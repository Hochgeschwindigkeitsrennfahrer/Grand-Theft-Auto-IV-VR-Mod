#include "log.h"
#include "openxr_bridge.h"
#include "openxr_controller.h"
#include "openxr_pose_client.h"
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
    asi::ShutdownOpenXrControllerBridge();
    asi::ShutdownOpenXrPoseBridge();
    asi::ShutdownOpenXrBridge();
    asi::OpenVrShutdown();
  }
  return TRUE;
}
