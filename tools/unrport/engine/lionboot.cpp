// Engine bring-up on the Lion: what Window/Src/Main.cpp InitEngine()/MainLoop() do on Windows.
#include "Engine.h"

static UEngine* GLionEngine;
static const DWORD kViFrame = 0x1F030010;
static DWORD GLastTickFrame;

extern "C" void lion_check_class_sizes();
void LionBuildMap(UGameEngine* Engine);  // editor/LionMap.cpp

// The stock Entry level is an empty room, so the arena is populated from code with classes of the game package:
// monsters and a few pickups. (Real maps place these as actors; the repository has no maps.)
static void SpawnArena() {
  ULevel* Level = ((UGameEngine*)GLionEngine)->GLevel;
  static const struct { const char* Class; FLOAT X, Y, Z; INT Yaw; } Actors[] = {
    {"UNreal.Krall", -80, -70, -90, 8192},      {"UNreal.Brute", 85, -85, -80, 24576},
    {"UNreal.Health", 40, 80, -118, 0},         {"UNreal.FlakCannon", -60, 60, -112, 0},
    {"UNreal.Eightball", 70, 20, -112, 0},      {"UNreal.ASMD", -20, -90, -112, 0},
  };
  for (INT i = 0; i < (INT)(sizeof(Actors) / sizeof(Actors[0])); ++i) {
    UClass* C = GObj.LoadClass(AActor::StaticClass, NULL, Actors[i].Class, NULL, LOAD_NoWarn | LOAD_KeepImports, NULL);
    if (!C) { debugf("arena: cannot load %s", Actors[i].Class); continue; }
    AActor* A = Level->SpawnActor(C, NAME_None, NULL, NULL, FVector(Actors[i].X, Actors[i].Y, Actors[i].Z), FRotator(0, Actors[i].Yaw, 0), NULL, 0, 0, 0);
    debugf("arena: %s -> %s", Actors[i].Class, A ? A->GetFullName() : "spawn failed");
  }
}

extern "C" void lion_engine_boot() {
  GSceneMem.Init(32768);
  GDynMem.Init(65536);
  UClass* EngineClass = GObj.LoadClass(UGameEngine::StaticClass, NULL, "ini:Engine.Engine.GameEngine", NULL, LOAD_NoFail | LOAD_KeepImports, NULL);
  GLionEngine = ConstructClassObject<UEngine>(EngineClass);
  lion_check_class_sizes();
  GLionEngine->Init();
  LionBuildMap((UGameEngine*)GLionEngine);
  GIsRunning = 1;
  GLastTickFrame = *(volatile DWORD*)kViFrame;
}

// Time and memory.
//
// The video frame counter is the game clock: every tick advances the world by the whole number of 60 Hz frames
// that went by since the previous tick (at least one, at most six), so motion matches what the screen shows even
// when a frame takes longer than 1/60 s, and a tick that finishes early waits for the next vblank (the CPU dozes,
// MSR[POW]; the handler at 0x500 only acknowledges the interrupt, see unrport.cpp).

static void WaitVblank() {
  const DWORD Frame = *(volatile DWORD*)kViFrame;
  DWORD Msr;
  asm volatile("mfmsr %0" : "=r"(Msr));
  const DWORD Doze = Msr | 0x8000 | 0x40000;  // EE | POW
  *(volatile DWORD*)0x1F040004 = 1;           // PIC: vblank only
  while (*(volatile DWORD*)kViFrame == Frame) asm volatile("mtmsr %0; isync" : : "r"(Doze));
  asm volatile("mtmsr %0; isync" : : "r"(Msr));
  *(volatile DWORD*)0x1F040004 = 0;
}

extern "C" void lion_heap_stats(DWORD* Used, DWORD* ArenaLeft);
extern "C" DWORD lion_heap_capacity();
extern "C" void lion_audio_trim();

// Called every few seconds: when the heap is nearly exhausted, collect unreferenced objects and trim the
// audio cache before the allocator has to give up.
static void WatchMemory(INT Ticks) {
  DWORD Used, Left;
  lion_heap_stats(&Used, &Left);
  static DWORD Peak;
  if (Used > Peak) Peak = Used;
  if (Ticks % 600 == 0) debugf("mem: %i KB in use, peak %i KB, arena left %i KB", (INT)(Used >> 10), (INT)(Peak >> 10), (INT)(Left >> 10));
  // Collect when little more than a megabyte of the heap is free (at most once every 20 s).
  static INT LastGc = -1200;
  if (Used + (3 << 19) > lion_heap_capacity() && Ticks - LastGc >= 1200) {
    LastGc = Ticks;
    GObj.CollectGarbage(GSystem, RF_Intrinsic);
    lion_audio_trim();
    lion_heap_stats(&Used, &Left);
    debugf("mem: low, collected: %i KB in use, arena left %i KB", (INT)(Used >> 10), (INT)(Left >> 10));
  }
}

// One iteration of MainLoop().
extern "C" void lion_engine_tick() {
  static INT Ticks;
  static DWORD LastPerf;
  const DWORD Vi = *(volatile DWORD*)kViFrame;
  DWORD Frames = Vi - GLastTickFrame;
  if (Frames < 1) Frames = 1;
  if (Frames > 6) Frames = 6;
  GLastTickFrame = Vi;
  if (++Ticks % 600 == 0) {
    debugf("perf: 600 ticks took %i video frames", (INT)(Vi - LastPerf));
    LastPerf = Vi;
  }
  WatchMemory(Ticks);
  GLionEngine->Tick((FLOAT)Frames * (1.0f / 60.0f));
  if (*(volatile DWORD*)kViFrame == Vi) WaitVblank();  // finished within the frame: wait for the next one
}
