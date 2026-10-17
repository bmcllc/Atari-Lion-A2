// Engine bring-up on the Lion: what Window/Src/Main.cpp InitEngine()/MainLoop() do on Windows.
#include "Engine.h"

static UEngine* GLionEngine;
static DOUBLE GLionOldTime;

extern "C" void lion_check_class_sizes();

extern "C" void lion_engine_boot() {
  GSceneMem.Init(32768);
  GDynMem.Init(65536);
  UClass* EngineClass = GObj.LoadClass(UGameEngine::StaticClass, NULL, "ini:Engine.Engine.GameEngine", NULL, LOAD_NoFail | LOAD_KeepImports, NULL);
  GLionEngine = ConstructClassObject<UEngine>(EngineClass);
  lion_check_class_sizes();
  GLionEngine->Init();
  GIsRunning = 1;
  GLionOldTime = appSeconds();
}

// One iteration of MainLoop(). The caller paces frames.
extern "C" void lion_engine_tick() {
  DOUBLE NewTime = appSeconds();
  GLionEngine->Tick((FLOAT)(NewTime - GLionOldTime));
  GLionOldTime = NewTime;
}
