// Platform layer replacing Unreal's Win32 UnPlat.cpp: logging, errors, and Core start-up.
#include "CorePrivate.h"

extern "C" void lion_halt(const char* why);
extern "C" void lion_run_ctors();

enum : DWORD { kLogAddr = 0x00380000, kLogSize = 0x10000 };

// Debug log ring in RAM; the emulator can dump it with --dump-ram (see tools/unrport/README).
static void log_text(const char* text) {
  volatile DWORD* len = (volatile DWORD*)kLogAddr;
  volatile char* buf = (volatile char*)(kLogAddr + 4);
  for (; *text; ++text)
    if (*len < kLogSize - 8) buf[(*len)++] = *text;
  if (*len < kLogSize - 8) buf[(*len)++] = '\n';
}

CORE_API DWORD appCycles() {
  DWORD tb;
  asm volatile("mftb %0" : "=r"(tb));
  return tb;
}

CORE_API void* appGetDllHandle(const char*) { return 0; }

CORE_API void VARARGS appErrorf(const char* Fmt, ...) {
  char Msg[1024];
  GET_VARARGS(Msg, Fmt);
  log_text("ERROR:");
  log_text(Msg);
  lion_halt("appErrorf");
}

void FGlobalPlatform::WriteBinary(const void* Data, INT, EName Event) {
  log_text(*FName(Event));
  log_text((const char*)Data);
}
void FGlobalPlatform::Warnf(const char* Fmt, ...) {
  char Msg[1024];
  GET_VARARGS(Msg, Fmt);
  log_text("WARN:");
  log_text(Msg);
}
UBOOL FGlobalPlatform::YesNof(const char*, ...) { return 1; }
void FGlobalPlatform::BeginSlowTask(const char*, UBOOL, UBOOL) {}
void FGlobalPlatform::EndSlowTask() {}
UBOOL VARARGS FGlobalPlatform::StatusUpdatef(INT, INT, const char*, ...) { return 1; }
UBOOL FGlobalPlatform::Exec(const char*, FOutputDevice*) { return 0; }

// Paths, config and the other Win32 services Core asks for: there is no disc layout or .ini on the Lion.
CORE_API const char* appCmdLine() { return ""; }
CORE_API const char* appBaseDir() { return ""; }
CORE_API const char* appLocalizationDir(bool) { return ""; }
CORE_API FGuid appCreateGuid() { FGuid G; appMemset(&G, 0, sizeof(G)); return G; }
CORE_API void* appGetDllExport(void*, const char*) { return 0; }
CORE_API UBOOL appMoveFile(const char*, const char*) { return 0; }
CORE_API DOUBLE appSeconds() { return appCycles() * (1.0 / 25000000.0); }  // time base = bus clock / 4
UBOOL appFindPackageFile(const char* In, const FGuid*, char* Out) {
  appStrcpy(Out, In);
  if (!appStrchr(In, '.')) appStrcat(Out, ".unr");
  return 1;
}
CORE_API UBOOL GetConfigString(const char*, const char*, char*, INT, const char*) { return 0; }
CORE_API UBOOL GetConfigSection(const char*, char*, INT, const char*) { return 0; }
CORE_API void SetConfigString(const char*, const char*, const char*, const char*) {}

// C++ exception ABI pieces that `throw`/`catch` reference. Nothing is ever unwound (see lionrt.cpp).
extern "C" {
void __cxa_free_exception(void*) {}
char _ZTIPc[16], _ZTIPKc[16];
}
namespace std { void terminate() { lion_halt("terminate"); } }
namespace __cxxabiv1 {
class __class_type_info { public: virtual ~__class_type_info(); };
__class_type_info::~__class_type_info() {}
}

// What appInit() does on Windows, minus the Windows: object manager, memory stack, platform hook.
extern "C" void lion_core_init() {
  lion_run_ctors();
  static FGlobalPlatform Platform;
  GSystem = &Platform;
  GIsClient = 1;  // selects which names and objects a package load keeps (RF_LoadForClient)
  GIsServer = 1;
  GObj.Init();
  GMem.Init(65536);
}
