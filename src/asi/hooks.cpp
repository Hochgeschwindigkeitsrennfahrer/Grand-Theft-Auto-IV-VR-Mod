#include "cam_matrix.h"
#include "log.h"
#include "openvr_mono.h"
#include "stereo_proj.h"
#include "stereo_render.h"

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
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

Direct3DCreate9_t g_realCreate9 = nullptr;
CreateDevice_t g_realCreateDevice = nullptr;
Present_t g_realPresent = nullptr;
EndScene_t g_realEndScene = nullptr;
Reset_t g_realReset = nullptr;

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
  // Frame finished — mono Submit to SteamVR, then EndScene
  asi::TryMonoSubmit(self);
  return g_realEndScene(self);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* self, const RECT* src, const RECT* dst,
                                      HWND hwnd, const RGNDATA* dirty) {
  return g_realPresent(self, src, dst, hwnd, dirty);
}

// Graphics options / resolution change call Reset — POOL_DEFAULT eye RTs die.
// Release BEFORE Reset to avoid hang on dangling StretchRect/Submit.
HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp) {
  const UINT bw = pp ? pp->BackBufferWidth : 0;
  const UINT bh = pp ? pp->BackBufferHeight : 0;
  asi::Log("DeviceReset: BEFORE Reset bb=%ux%u — releasing eye RTs", bw, bh);
  asi::StereoNotifyDeviceLost();
  const HRESULT hr = g_realReset(self, pp);
  // Always clear again after Reset — CreateTexture may have raced mid-call.
  asi::StereoNotifyDeviceLost();
  if (SUCCEEDED(hr))
    asi::Log("DeviceReset: OK — eye RTs will recreate on next EndScene");
  else
    asi::Log("DeviceReset: FAILED hr=0x%08lx (eye RTs cleared; wait for next Reset)",
             static_cast<unsigned long>(hr));
  return hr;
}

void EnsureDeviceHooks(IDirect3DDevice9* device) {
  void** vt = *reinterpret_cast<void***>(device);
  // IDirect3DDevice9: Reset=16, Present=17, EndScene=42
  HookFn(vt[16], reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&g_realReset),
         "Reset");
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

  return HookFn(proc, reinterpret_cast<void*>(&HookDirect3DCreate9),
                reinterpret_cast<void**>(&g_realCreate9), "Direct3DCreate9");
}

DWORD WINAPI HookThread(LPVOID) {
  for (int i = 0; i < 200; ++i) {
    if (GetModuleHandleA("d3d9.dll")) {
      if (!InstallCreate9Hook())
        return 1;
      // Cam matrix hooks install AFTER OpenVR submit warmup (see openvr_mono) —
      // installing during title causes blackscreen/freeze.
      Sleep(1500);
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
