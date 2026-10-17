// Unreal's audio subsystem on the Tyke DSP.
//
// The Tyke runs a mixing firmware (see docs/hardware.md): per voice START/LENGTH/LOOP_START/PITCH/VOLUME/CTRL
// registers in its data RAM, PCM16 big-endian samples in SDRAM. Sound objects carry no data in memory: the
// engine patch stores where the wave lies in the package file, and the first PlaySound reads it from the disc,
// converts it to mono PCM16 and keeps it in a cache of a few megabytes (least recently used evicted).
#include "Inc_LionDrv.h"
#include "UnLinker.h"

namespace {

enum { kTyke = 0x1F020000, kVoices = 24, kCacheBytes = 1 << 20, kMaxBufs = 96, kAmbientSlot = 7 };

inline volatile DWORD* TykeReg(DWORD Offset) { return (volatile DWORD*)(kTyke + Offset); }
inline volatile DWORD* VoiceReg(INT Ch, DWORD Reg) { return TykeReg(0x100 + 0x20 * Ch + Reg); }
enum { RStart = 0, RLength = 4, RLoop = 8, RPitch = 12, RVolume = 16, RCtrl = 20 };

struct SoundInfo {
  DWORD Offset, Size;
  char File[24];
};

struct Buf {
  USound* Sound;
  SWORD* Pcm;
  INT Samples;
  DWORD Rate;
  DWORD Stamp;
};

struct Voice {
  AActor* Actor;
  INT Slot;
  Buf* B;
  FVector Location;
  FLOAT Volume, Radius, Pitch;
  UBOOL Spatial;
  DWORD Stamp;
  INT Tick;
};

Buf GBufs[kMaxBufs];
INT GCacheUsed;
Voice GVoices[kVoices];
DWORD GClock;
INT GTick;
FILE* GFile;
char GFileName[24];

inline DWORD Le16(const BYTE* p) { return p[0] | (p[1] << 8); }
inline DWORD Le32(const BYTE* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((DWORD)p[3] << 24); }

UBOOL VoiceBusy(INT Ch) { return (*VoiceReg(Ch, RCtrl) & 1) != 0; }

UBOOL InUse(const Buf* B) {
  for (INT i = 0; i < kVoices; ++i)
    if (GVoices[i].B == B && VoiceBusy(i)) return 1;
  return 0;
}

void FreeBuf(Buf* B) {
  if (B->Pcm) appFree(B->Pcm);
  GCacheUsed -= B->Samples * 2;
  B->Pcm = NULL;
  B->Sound = NULL;
  B->Samples = 0;
}

// The engine patch left the file offset in Handle and the size in OriginalSize; the first use turns them
// into a SoundInfo (sounds are never registered with the device before they play).
SoundInfo* InfoOf(USound* Sound) {
  if (Sound->OriginalSize > 0) {
    SoundInfo* Info = (SoundInfo*)appMalloc(sizeof(SoundInfo), "sndinfo");
    Info->Offset = (DWORD)Sound->Handle;
    Info->Size = (DWORD)Sound->OriginalSize;
    appStrcpy(Info->File, "UNreal.u");
    if (ULinkerLoad* Linker = Sound->GetLinker()) {
      appStrncpy(Info->File, Linker->LinkerRoot->GetName(), 16);
      appStrcat(Info->File, ".u");
    }
    Sound->Handle = Info;
    Sound->OriginalSize = 0;
  }
  return (SoundInfo*)Sound->Handle;
}

// Reads and decodes the sound; returns its cache entry or NULL.
Buf* Fetch(USound* Sound) {
  SoundInfo* Info = InfoOf(Sound);
  if (!Info || !Info->Size) return NULL;
  for (INT i = 0; i < kMaxBufs; ++i)
    if (GBufs[i].Sound == Sound) { GBufs[i].Stamp = ++GClock; return &GBufs[i]; }

  if (!GFile || appStricmp(GFileName, Info->File)) {
    if (GFile) appFclose(GFile);
    GFile = appFopen(Info->File, "rb");
    appStrcpy(GFileName, Info->File);
    if (!GFile) return NULL;
  }
  if (Info->Size > (1 << 20)) return NULL;
  BYTE* Raw = (BYTE*)appMalloc(Info->Size, "snd");
  if (!Raw) return NULL;
  appFseek(GFile, Info->Offset, 0);
  if (appFread(Raw, 1, Info->Size, GFile) != (INT)Info->Size) { appFree(Raw); return NULL; }

  // RIFF/WAVE: fmt and data chunks.
  DWORD Channels = 0, Rate = 0, Bits = 0, DataAt = 0, DataLen = 0;
  if (Info->Size > 12 && !appMemcmp(Raw, "RIFF", 4)) {
    DWORD p = 12;
    while (p + 8 <= Info->Size) {
      const DWORD Len = Le32(Raw + p + 4);
      if (!appMemcmp(Raw + p, "fmt ", 4) && p + 24 <= Info->Size) {
        Channels = Le16(Raw + p + 10);
        Rate = Le32(Raw + p + 12);
        Bits = Le16(Raw + p + 22);
      } else if (!appMemcmp(Raw + p, "data", 4)) {
        DataAt = p + 8;
        DataLen = Len;
        break;
      }
      p += 8 + Len + (Len & 1);
    }
  }
  if (!DataAt || (Channels != 1 && Channels != 2) || (Bits != 8 && Bits != 16) || Rate < 4000) { appFree(Raw); return NULL; }
  if (DataAt + DataLen > Info->Size) DataLen = Info->Size - DataAt;
  const INT Frame = Channels * Bits / 8;
  const INT Samples = DataLen / Frame;
  if (Samples < 2) { appFree(Raw); return NULL; }

  // Make room, evicting the least recently used sounds that are not playing.
  while (GCacheUsed + Samples * 2 > kCacheBytes) {
    Buf* Old = NULL;
    for (INT i = 0; i < kMaxBufs; ++i)
      if (GBufs[i].Pcm && !InUse(&GBufs[i]) && (!Old || GBufs[i].Stamp < Old->Stamp)) Old = &GBufs[i];
    if (!Old) { appFree(Raw); return NULL; }
    FreeBuf(Old);
  }
  Buf* B = NULL;
  for (INT i = 0; i < kMaxBufs && !B; ++i)
    if (!GBufs[i].Pcm) B = &GBufs[i];
  SWORD* Pcm = B ? (SWORD*)appMalloc(Samples * 2, "pcm") : NULL;
  if (!Pcm) { appFree(Raw); return NULL; }
  const BYTE* S = Raw + DataAt;
  for (INT i = 0; i < Samples; ++i) {
    INT V = 0;
    for (DWORD c = 0; c < Channels; ++c) {
      if (Bits == 8) V += ((INT)S[i * Frame + c] - 128) << 8;
      else V += (SWORD)Le16(S + i * Frame + c * 2);
    }
    Pcm[i] = (SWORD)(V / (INT)Channels);
  }
  appFree(Raw);
  B->Sound = Sound;
  B->Pcm = Pcm;
  B->Samples = Samples;
  B->Rate = Rate;
  B->Stamp = ++GClock;
  GCacheUsed += Samples * 2;
  return B;
}

void StopVoice(INT Ch) {
  *VoiceReg(Ch, RCtrl) = 0;
  GVoices[Ch].B = NULL;
  GVoices[Ch].Actor = NULL;
}

// Distance attenuation and panning against the listener.
void Mix(INT Ch, const FCoords* Listener) {
  Voice& V = GVoices[Ch];
  FLOAT L = V.Volume, R = V.Volume;
  if (V.Spatial && Listener) {
    const FVector D = V.Location - Listener->Origin;
    const FLOAT Dist = D.Size();
    FLOAT Atten = 1.0f - Dist / V.Radius;
    if (Atten < 0) Atten = 0;
    Atten *= Atten * 0.5f + 0.5f;
    FLOAT Pan = 0;
    if (Dist > 16.0f) Pan = (D | Listener->YAxis) / Dist;
    L = V.Volume * Atten * (Pan > 0 ? 1.0f - 0.7f * Pan : 1.0f);
    R = V.Volume * Atten * (Pan < 0 ? 1.0f + 0.7f * Pan : 1.0f);
  }
  INT Li = (INT)(L * 256.0f), Ri = (INT)(R * 256.0f);
  Li = Li < 0 ? 0 : Li > 256 ? 256 : Li;
  Ri = Ri < 0 ? 0 : Ri > 256 ? 256 : Ri;
  *VoiceReg(Ch, RVolume) = ((DWORD)Li << 16) | (DWORD)Ri;
}

}  // namespace

ULionAudioSubsystem::ULionAudioSubsystem() : Viewport(NULL), Ticks(0) {}

UBOOL ULionAudioSubsystem::Init() {
  USound::Audio = this;
  UMusic::Audio = this;
  *TykeReg(0) = 0x100;  // master volume
  for (INT i = 0; i < kVoices; ++i) StopVoice(i);
  debugf(NAME_Init, "Lion audio initialized (Tyke running: %i)", (INT)(*(volatile DWORD*)(kTyke + 0x8000) & 1));
  return 1;
}

void ULionAudioSubsystem::SetViewport(UViewport* InViewport) { Viewport = InViewport; }
UBOOL ULionAudioSubsystem::Exec(const char*, FOutputDevice*) { return 0; }

void ULionAudioSubsystem::RegisterSound(USound*) {}

void ULionAudioSubsystem::UnregisterSound(USound* Sound) {
  for (INT i = 0; i < kMaxBufs; ++i)
    if (GBufs[i].Sound == Sound) {
      for (INT v = 0; v < kVoices; ++v)
        if (GVoices[v].B == &GBufs[i]) StopVoice(v);
      FreeBuf(&GBufs[i]);
    }
  if (Sound->OriginalSize == 0 && Sound->Handle) appFree(Sound->Handle);
  Sound->Handle = NULL;
}

UBOOL ULionAudioSubsystem::PlaySound(AActor* Actor, INT Id, USound* Sound, FVector Location, FLOAT Volume, FLOAT Radius, FLOAT Pitch) {
  if (!Sound || !InfoOf(Sound)) return 0;
  // The engine plays some sounds twice in one tick (once per role): keep one.
  for (INT i = 0; i < kVoices; ++i)
    if (GVoices[i].B && GVoices[i].B->Sound == Sound && GVoices[i].Actor == Actor && GVoices[i].Tick == GTick) return 1;
  const INT Slot = (Id >> 1) & 7;
  const UBOOL NoOverride = Id & 1;
  // The same actor and slot replace each other unless the sound asks not to be overridden.
  INT Free = -1;
  for (INT i = 0; i < kVoices; ++i) {
    const UBOOL Busy = VoiceBusy(i);
    if (Busy && Slot != 0 && GVoices[i].Actor == Actor && GVoices[i].Slot == Slot) {
      if (NoOverride) return 0;
      Free = i;
      break;
    }
  }
  if (Free < 0) {
    for (INT i = 0; i < kVoices; ++i)
      if (!VoiceBusy(i)) { Free = i; break; }
  }
  if (Free < 0) {  // steal the oldest non-ambient voice
    DWORD Oldest = 0xFFFFFFFF;
    for (INT i = 0; i < kVoices; ++i)
      if (GVoices[i].Slot != kAmbientSlot && GVoices[i].Stamp < Oldest) { Oldest = GVoices[i].Stamp; Free = i; }
  }
  if (Free < 0) return 0;

  Buf* B = Fetch(Sound);
  if (!B) return 0;
  Voice& V = GVoices[Free];
  *VoiceReg(Free, RCtrl) = 0;
  V.Actor = Actor;
  V.Slot = Slot;
  V.B = B;
  V.Location = Location;
  V.Volume = Volume > 4.0f ? 4.0f : Volume;
  V.Radius = Radius > 0 ? Radius : 1600.0f;
  V.Pitch = Pitch > 0 ? Pitch : 1.0f;
  V.Spatial = Actor != NULL && Location != FVector(0, 0, 0);
  V.Stamp = ++GClock;
  V.Tick = GTick;
  Mix(Free, NULL);  // refined by Update
  *VoiceReg(Free, RStart) = (DWORD)B->Pcm;
  *VoiceReg(Free, RLength) = B->Samples;
  *VoiceReg(Free, RLoop) = 0;
  *VoiceReg(Free, RPitch) = (DWORD)((FLOAT)B->Rate * V.Pitch * 65536.0f / 44100.0f);
  *VoiceReg(Free, RCtrl) = 1;
  static INT Logged;
  if (Logged++ < 40) debugf("audio: %s voice %i %i samples at %i Hz, volume %f", Sound->GetName(), Free, B->Samples, (INT)B->Rate, Volume);
  return 1;
}

void ULionAudioSubsystem::Update(FPointRegion, FCoords& Listener) {
  ++Ticks;
  GTick = Ticks;
  for (INT i = 0; i < kVoices; ++i) {
    if (!GVoices[i].B) continue;
    if (!VoiceBusy(i)) { GVoices[i].B = NULL; GVoices[i].Actor = NULL; continue; }
    // Moving emitters follow their actor.
    if (GVoices[i].Actor && GVoices[i].Spatial && !GVoices[i].Actor->bDeleteMe) GVoices[i].Location = GVoices[i].Actor->Location;
    Mix(i, &Listener);
  }
  // Looping ambient sounds of actors near the listener, refreshed a few times a second.
  if (Ticks % 12 == 0 && Viewport && Viewport->Actor && Viewport->Actor->XLevel) {
    ULevel* Level = Viewport->Actor->XLevel;
    for (INT a = 0; a < Level->Num(); ++a) {
      AActor* A = Level->Actors(a);
      if (!A || !A->AmbientSound || A->bDeleteMe || !InfoOf(A->AmbientSound)) continue;
      const FLOAT Radius = A->WorldSoundRadius();
      if (FDistSquared(A->Location, Listener.Origin) > Radius * Radius) continue;
      INT Have = -1;
      for (INT i = 0; i < kVoices; ++i)
        if (GVoices[i].Actor == A && GVoices[i].Slot == kAmbientSlot && VoiceBusy(i)) Have = i;
      if (Have >= 0) continue;
      INT Free = -1;
      for (INT i = 0; i < kVoices && Free < 0; ++i)
        if (!VoiceBusy(i)) Free = i;
      if (Free < 0) break;
      Buf* B = Fetch(A->AmbientSound);
      if (!B) continue;
      Voice& V = GVoices[Free];
      V.Actor = A; V.Slot = kAmbientSlot; V.B = B; V.Location = A->Location;
      V.Volume = A->SoundVolume / 128.0f; V.Radius = Radius; V.Pitch = A->SoundPitch / 64.0f; V.Spatial = 1; V.Stamp = ++GClock;
      Mix(Free, &Listener);
      *VoiceReg(Free, RStart) = (DWORD)B->Pcm;
      *VoiceReg(Free, RLength) = B->Samples;
      *VoiceReg(Free, RLoop) = 0;
      *VoiceReg(Free, RPitch) = (DWORD)((FLOAT)B->Rate * V.Pitch * 65536.0f / 44100.0f);
      *VoiceReg(Free, RCtrl) = 3;  // KEY_ON | LOOP
    }
    // Ambient voices whose actor left the radius or went away stop.
    for (INT i = 0; i < kVoices; ++i)
      if (GVoices[i].Slot == kAmbientSlot && GVoices[i].Actor && VoiceBusy(i)) {
        AActor* A = GVoices[i].Actor;
        const FLOAT Radius = A->WorldSoundRadius();
        if (A->bDeleteMe || !A->AmbientSound || FDistSquared(A->Location, Listener.Origin) > Radius * Radius) StopVoice(i);
      }
  }
}

void ULionAudioSubsystem::NoteDestroy(AActor* Actor) {
  for (INT i = 0; i < kVoices; ++i)
    if (GVoices[i].Actor == Actor) {
      if (GVoices[i].Slot == kAmbientSlot) StopVoice(i);
      else GVoices[i].Actor = NULL;  // let the sound finish, without following the actor
    }
}

IMPLEMENT_CLASS(ULionAudioSubsystem);

// Drops every cached sound that is not playing (called when memory runs low).
extern "C" void lion_audio_trim() {
  for (INT i = 0; i < kMaxBufs; ++i)
    if (GBufs[i].Pcm && !InUse(&GBufs[i])) FreeBuf(&GBufs[i]);
}
