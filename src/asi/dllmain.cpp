#include "log.h"
#include "openvr_mono.h"

#include <windows.h>

void StartHooks();

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    asi::LogInit();
    StartHooks();
  } else if (reason == DLL_PROCESS_DETACH) {
    asi::OpenVrShutdown();
  }
  return TRUE;
}
