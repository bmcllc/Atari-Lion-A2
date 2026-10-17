// Engine bring-up on the Lion: what Window/Src/Main.cpp InitEngine()/MainLoop() do on Windows.
#include "Engine.h"

static UEngine* GLionEngine;
static DOUBLE GLionOldTime;

extern "C" void lion_check_class_sizes();

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
  SpawnArena();
  GIsRunning = 1;
  GLionOldTime = appSeconds();
}

// Sleep until the next video frame: the CPU dozes (MSR[POW]) and the vblank interrupt wakes it; the
// handler at 0x500 only acknowledges the interrupt (see unrport.cpp).
static void WaitVblank() {
  const DWORD Frame = *(volatile DWORD*)0x1F030010;
  DWORD Msr;
  asm volatile("mfmsr %0" : "=r"(Msr));
  const DWORD Doze = Msr | 0x8000 | 0x40000;  // EE | POW
  *(volatile DWORD*)0x1F040004 = 1;           // PIC: vblank only
  while (*(volatile DWORD*)0x1F030010 == Frame) asm volatile("mtmsr %0; isync" : : "r"(Doze));
  asm volatile("mtmsr %0; isync" : : "r"(Msr));
  *(volatile DWORD*)0x1F040004 = 0;
}

// One iteration of MainLoop(). The caller paces frames.
extern "C" void lion_engine_tick() {
  // Frame-rate readout: video frames (60 Hz) per 16 engine ticks, in the log.
  static INT Ticks;
  static DWORD LastVi;
  if (++Ticks % 600 == 0) {
    const DWORD Vi = *(volatile DWORD*)0x1F030010;
    debugf("perf: 600 ticks took %i video frames", (INT)(Vi - LastVi));
    LastVi = Vi;
  }
  DOUBLE NewTime = appSeconds();
  GLionEngine->Tick((FLOAT)(NewTime - GLionOldTime));
  GLionOldTime = NewTime;
  WaitVblank();  // 60 Hz
}
