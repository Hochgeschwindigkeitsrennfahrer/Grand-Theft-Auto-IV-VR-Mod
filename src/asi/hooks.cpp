#include "cam_matrix.h"
#include "log.h"
#include "openxr_bridge.h"
#include "openxr_pose_client.h"
#include "openvr_mono.h"
#include "stereo_proj.h"

#include "../../thirdparty/minhook/include/MinHook.h"

#include <d3d9.h>
#include <windows.h>

#include <mutex>
#include <unordered_set>

namespace {

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                              const RGNDATA*);
using EndScene_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

Direct3DCreate9_t g_realCreate9 = nullptr;
CreateDevice_t g_realCreateDevice = nullptr;
Present_t g_realPresent = nullptr;
EndScene_t g_realEndScene = nullptr;

std::mutex g_hookMu;
std::unordered_set<void*> g_hookedFns;
bool g_mhReady = false;

bool HookFn(void* target, void* detour, void** original, const char* name) {
  std::lock_guard<std::mutex> lock(g_hookMu);
  if (g_hookedFns.count(target))
    return true;
  if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
    asi::Log("Hook FAILED %s target=%p", name, target);
    return false;
  }
  g_hookedFns.insert(target);
  asi::Log("Hooked %s target=%p", name, target);
  return true;
}

HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* self) {
  // Frame finished: use exactly one compositor backend.
  if (asi::IsOpenXrBridgeRequested()) {
    asi::PollOpenXrPoseBridge();
    asi::PublishOpenXrFrame(self);
  } else
    asi::TryMonoSubmit(self);
  return g_realEndScene(self);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* self, const RECT* src, const RECT* dst,
                                      HWND hwnd, const RGNDATA* dirty) {
  return g_realPresent(self, src, dst, hwnd, dirty);
}

void EnsureDeviceHooks(IDirect3DDevice9* device) {
  void** vt = *reinterpret_cast<void***>(device);
  HookFn(vt[17], reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_realPresent),
         "Present");
  HookFn(vt[42], reinterpret_cast<void*>(&HookEndScene), reinterpret_cast<void**>(&g_realEndScene),
         "EndScene");
  asi::InstallStereoProjHooks(device);
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type,
                                           HWND hwnd, DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice9** out) {
  const HRESULT hr = g_realCreateDevice(self, adapter, type, hwnd, flags, pp, out);
  if (FAILED(hr) || !out || !*out) {
    asi::Log("CreateDevice FAILED hr=0x%08lx", static_cast<unsigned long>(hr));
    return hr;
  }
  asi::Log("CreateDevice OK device=%p", static_cast<void*>(*out));
  EnsureDeviceHooks(*out);
  return hr;
}

void EnsureCreateDeviceHook(IDirect3D9* obj) {
  void** vt = *reinterpret_cast<void***>(obj);
  HookFn(vt[16], reinterpret_cast<void*>(&HookCreateDevice),
         reinterpret_cast<void**>(&g_realCreateDevice), "CreateDevice");
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdk) {
  asi::Log("Direct3DCreate9 sdk=%u", sdk);
  IDirect3D9* obj = g_realCreate9(sdk);
  if (!obj) {
    asi::Log("Direct3DCreate9 returned null");
    return obj;
  }
  EnsureCreateDeviceHook(obj);
  return obj;
}

LRESULT CALLBACK ProbeWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  return DefWindowProcA(window, message, wParam, lParam);
}

bool PrimeExistingDeviceHooks() {
  if (!g_realCreate9)
    return false;

  constexpr const char* kClassName = "GTAIV_DXVK_VR_HookProbe";
  WNDCLASSEXA windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = ProbeWindowProc;
  windowClass.hInstance = GetModuleHandleA(nullptr);
  windowClass.lpszClassName = kClassName;
  if (!RegisterClassExA(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    asi::Log("Hook probe RegisterClassEx failed win32=%lu", GetLastError());
    return false;
  }

  HWND window = CreateWindowExA(0, kClassName, "", WS_OVERLAPPED, 0, 0, 16, 16, nullptr,
                                nullptr, windowClass.hInstance, nullptr);
  if (!window) {
    asi::Log("Hook probe CreateWindowEx failed win32=%lu", GetLastError());
    return false;
  }

  IDirect3D9* direct3D = g_realCreate9(D3D_SDK_VERSION);
  if (!direct3D) {
    DestroyWindow(window);
    asi::Log("Hook probe Direct3DCreate9 returned null");
    return false;
  }
  EnsureCreateDeviceHook(direct3D);

  D3DPRESENT_PARAMETERS parameters{};
  parameters.Windowed = TRUE;
  parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
  parameters.BackBufferFormat = D3DFMT_UNKNOWN;
  parameters.BackBufferWidth = 16;
  parameters.BackBufferHeight = 16;
  parameters.hDeviceWindow = window;

  IDirect3DDevice9* probeDevice = nullptr;
  const HRESULT result = direct3D->CreateDevice(
      D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters,
      &probeDevice);
  if (probeDevice)
    probeDevice->Release();
  direct3D->Release();
  DestroyWindow(window);

  if (FAILED(result)) {
    asi::Log("Hook probe CreateDevice failed hr=0x%08lx", static_cast<unsigned long>(result));
    return false;
  }
  asi::Log("Hook probe armed shared DXVK EndScene/Present vtable");
  return true;
}

bool InstallCreate9Hook() {
  HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
  if (!d3d9)
    d3d9 = LoadLibraryA("d3d9.dll");
  if (!d3d9) {
    asi::Log("d3d9.dll missing");
    return false;
  }

  auto* proc = reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9"));
  if (!proc) {
    asi::Log("Direct3DCreate9 export missing");
    return false;
  }

  if (!g_mhReady) {
    if (MH_Initialize() != MH_OK) {
      asi::Log("MH_Initialize failed");
      return false;
    }
    g_mhReady = true;
  }

  if (!HookFn(proc, reinterpret_cast<void*>(&HookDirect3DCreate9),
              reinterpret_cast<void**>(&g_realCreate9), "Direct3DCreate9"))
    return false;
  PrimeExistingDeviceHooks();
  return true;
}

DWORD WINAPI HookThread(LPVOID) {
  for (int i = 0; i < 200; ++i) {
    if (GetModuleHandleA("d3d9.dll")) {
      if (!InstallCreate9Hook())
        return 1;
      // Cam matrix hooks install AFTER OpenVR submit warmup (see openvr_mono) —
      // installing during title causes blackscreen/freeze.
      Sleep(1500);
      if (asi::IsOpenXrBridgeRequested())
        asi::Log("OpenXRBridge: game hook armed; waiting for first EndScene");
      else
        asi::InitOpenVrEarly();
      return 0;
    }
    Sleep(50);
  }
  asi::Log("Gave up waiting for d3d9.dll");
  return 1;
}

}  // namespace

void StartHooks() {
  CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
}
