// Entry point of the Unreal port: start Unreal's Core and Engine on the Lion and run the game.
// Everything else lives in the packages: Core/Engine/Render are Epic's code compiled for PowerPC (core/,
// engine/), LionDrv (liondrv/) is the viewport, render device and input.
extern "C" void lion_core_init();
extern "C" void lion_engine_test();
extern "C" void lion_engine_boot();
extern "C" void lion_engine_tick();

extern "C" int main(void) {
  lion_core_init();
  lion_engine_test();
  lion_engine_boot();
  for (;;) lion_engine_tick();
}
