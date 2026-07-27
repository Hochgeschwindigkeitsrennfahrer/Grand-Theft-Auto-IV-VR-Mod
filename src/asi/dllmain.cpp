#include "log.h"
#include "openvr_mono.h"
#include "perf_debug.h"

#include <windows.h>

void StartHooks();

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    asi::LogInit();
    asi::PerfDebugInit();
    StartHooks();
  } else if (reason == DLL_PROCESS_DETACH) {
    asi::OpenVrShutdown();
  }
  return TRUE;
}
