#include "stereo_render.h"
#include "aob.h"
#include "cam_matrix.h"
#include "hmd_pose.h"
#include "log.h"
#include "stereo_config.h"
#include "stereo_eye.h"
#include "stereo_proj.h"
#include "vr_display.h"

#include "../../thirdparty/dxvk/d3d9_vk_interop.h"
#include "../../thirdparty/minhook/include/MinHook.h"

#include <d3d9.h>

#include <atomic>
#include <cmath>
#include <cstdint>

#include <openvr.h>

namespace asi {
namespace {

using BuildRenderList_t = void(__fastcall*)(void* self, void* edx);

BuildRenderList_t g_origBuild = nullptr;
BuildRenderList_t g_origPhaseA = nullptr;
BuildRenderList_t g_origPhaseC = nullptr;
BuildRenderList_t g_origExecA = nullptr;
BuildRenderList_t g_origExecC = nullptr;
BuildRenderList_t g_origExecD = nullptr;

std::atomic<bool> g_ok{false};
std::atomic<bool> g_inDual{false};
std::atomic<uint32_t> g_passCount{0};
std::atomic<uint32_t> g_frameSeq{0};
std::atomic<uint32_t> g_phaseLogLeft{40};
std::atomic<uint32_t> g_execDualCount{0};

void* g_lastThisDraw = nullptr;
void* g_lastThisPhaseA = nullptr;
void* g_lastThisPhaseC = nullptr;

// Mode 6/7/8/10: dualDone prevents a second orchestration in the same EndScene epoch.
bool g_dualDoneThisFrame = false;
int g_skipExecC = 0;
int g_skipExecD = 0;
int g_skipBuildC = 0;
int g_skipBuildD = 0;
int g_skipExecA = 0;
bool g_loggedRt = false;
bool g_execAHooked = false;
bool g_execCHooked = false;
bool g_execDHooked = false;
// Proj inject window: natural Left PhaseA..DrawScene + inDual Right pass.
std::atomic<bool> g_projWindow{false};

IDirect3DDevice9* g_device = nullptr;
IDirect3DTexture9* g_texL = nullptr;
IDirect3DTexture9* g_texR = nullptr;
uint32_t g_rtW = 0, g_rtH = 0;

bool g_haveL = false;
bool g_haveR = false;
bool g_loggedIpd = false;
std::atomic<uint32_t> g_temporalFrames{0};
std::atomic<uint32_t> g_sameFrameCount{0};

void ReleaseEyeRts() {
  if (g_texL) {
    g_texL->Release();
    g_texL = nullptr;
  }
  if (g_texR) {
    g_texR->Release();
    g_texR = nullptr;
  }
  g_rtW = g_rtH = 0;
  g_haveL = g_haveR = false;
}

bool EnsureEyeRts(IDirect3DDevice9* dev, uint32_t w, uint32_t h) {
  if (!dev || w < 16 || h < 16)
    return false;
  if (g_texL && g_texR && g_device == dev && g_rtW == w && g_rtH == h)
    return true;
  ReleaseEyeRts();
  g_device = dev;
  if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                &g_texL, nullptr)) ||
      FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                &g_texR, nullptr))) {
    ReleaseEyeRts();
    Log("StereoRender: CreateTexture RT FAIL %ux%u", w, h);
    return false;
  }
  g_rtW = w;
  g_rtH = h;
  Log("StereoRender: eye RTs %ux%u OK", w, h);
  return true;
}

bool CopyBbToEye(IDirect3DDevice9* dev, IDirect3DTexture9* tex) {
  if (!dev || !tex)
    return false;
  IDirect3DSurface9* bb = nullptr;
  IDirect3DSurface9* dst = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    return false;
  if (FAILED(tex->GetSurfaceLevel(0, &dst)) || !dst) {
    bb->Release();
    return false;
  }
  const HRESULT hr = dev->StretchRect(bb, nullptr, dst, nullptr, D3DTEXF_NONE);
  dst->Release();
  bb->Release();
  return SUCCEEDED(hr);
}

// Prefer current color RT (offscreen); fall back to backbuffer.
bool CopyCurrentColorToEye(IDirect3DDevice9* dev, IDirect3DTexture9* tex) {
  if (!dev || !tex)
    return false;
  IDirect3DSurface9* src = nullptr;
  if (FAILED(dev->GetRenderTarget(0, &src)) || !src)
    return CopyBbToEye(dev, tex);

  if (!g_loggedRt) {
    D3DSURFACE_DESC d{};
    if (SUCCEEDED(src->GetDesc(&d))) {
      IDirect3DSurface9* bb = nullptr;
      const bool isBb =
          SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb == src;
      if (bb)
        bb->Release();
      Log("StereoRT: GetRenderTarget0 %ux%u fmt=%u isBB=%d", d.Width, d.Height,
          static_cast<unsigned>(d.Format), isBb ? 1 : 0);
      g_loggedRt = true;
    }
  }

  IDirect3DSurface9* dst = nullptr;
  if (FAILED(tex->GetSurfaceLevel(0, &dst)) || !dst) {
    src->Release();
    return false;
  }
  const HRESULT hr = dev->StretchRect(src, nullptr, dst, nullptr, D3DTEXF_NONE);
  dst->Release();
  src->Release();
  if (SUCCEEDED(hr))
    return true;
  return CopyBbToEye(dev, tex);
}

bool IsSameFrameMode(StereoMode mode) {
  return mode == StereoMode::SameFrameDual || mode == StereoMode::GBufferRtDual ||
         mode == StereoMode::FusionSwap;
}

bool UsesRtAndProj(StereoMode mode) {
  return mode == StereoMode::GBufferRtDual || mode == StereoMode::FusionSwap;
}

bool IsExecuteDualMode(StereoMode mode) {
  return mode == StereoMode::ExecuteDual || mode == StereoMode::D3dCamDual;
}

bool IsBuildExecDualMode(StereoMode mode) {
  return mode == StereoMode::BuildExecDual;
}

bool NeedsExecHooks(StereoMode mode) {
  return IsExecuteDualMode(mode) || IsBuildExecDualMode(mode);
}

bool WantsD3dCamPush(StereoMode mode) {
  return mode == StereoMode::D3dCamDual;
}

bool IsStubVtblFn(void* fn, void** vt) {
  if (!fn || !vt)
    return true;
  if (fn == vt[1] || fn == vt[2])
    return true;
  return false;
}

bool HookOneBuild(const char* label, uintptr_t addr, void* detour, BuildRenderList_t* origOut);

void TryInstallExecFromVtbl(void* self, int execIdx, bool* hookedFlag, BuildRenderList_t* origOut,
                            void* detour, const char* label) {
  if (*hookedFlag || !self)
    return;
  void** vt = *reinterpret_cast<void***>(self);
  if (!vt) {
    *hookedFlag = true;
    return;
  }
  void* fn = vt[execIdx];
  if (IsStubVtblFn(fn, vt)) {
    Log("StereoExec: %s vt[%d]=%p is stub — skip", label, execIdx, fn);
    *hookedFlag = true;
    return;
  }
  if (HookOneBuild(label, reinterpret_cast<uintptr_t>(fn), detour, origOut))
    Log("StereoExec: hooked %s @ %p (vt[%d])", label, fn, execIdx);
  *hookedFlag = true;
}

void RunExecuteEyePass(void* edx) {
  if (g_origExecA && g_lastThisPhaseA)
    g_origExecA(g_lastThisPhaseA, edx);
  if (g_origExecC && g_lastThisPhaseC)
    g_origExecC(g_lastThisPhaseC, edx);
  if (g_origExecD && g_lastThisDraw)
    g_origExecD(g_lastThisDraw, edx);
}

void RunBuildEyePass(void* edx) {
  if (g_origPhaseA && g_lastThisPhaseA)
    g_origPhaseA(g_lastThisPhaseA, edx);
  if (g_origPhaseC && g_lastThisPhaseC)
    g_origPhaseC(g_lastThisPhaseC, edx);
  if (g_origBuild && g_lastThisDraw)
    g_origBuild(g_lastThisDraw, edx);
}

// Mode 11: from PhaseA *Build* — cam → Build → Execute per eye (never Build-from-Exec).
void RunBuildExecDualFromPhaseABuild(void* edx) {
  if (!g_origPhaseA || !g_origExecA || !g_origExecD || !g_device || !g_texL || !g_texR)
    return;
  if (!g_lastThisPhaseC || !g_lastThisDraw)
    return;

  float lx = 0.f, ly = 0.f, lz = 0.f, rx = 0.f, ry = 0.f, rz = 0.f;
  g_inDual.store(true);

  SetStereoEye(StereoEye::Left);
  RefreshLiveCamForStereoEye();
  GetLastStereoCamPos(&lx, &ly, &lz);
  RunBuildEyePass(edx);
  RunExecuteEyePass(edx);
  if (CopyCurrentColorToEye(g_device, g_texL))
    g_haveL = true;

  SetStereoEye(StereoEye::Right);
  RefreshLiveCamForStereoEye();
  GetLastStereoCamPos(&rx, &ry, &rz);
  RunBuildEyePass(edx);
  RunExecuteEyePass(edx);
  if (CopyCurrentColorToEye(g_device, g_texR))
    g_haveR = true;

  SetStereoEye(StereoEye::Left);
  RefreshLiveCamForStereoEye();
  g_inDual.store(false);
  g_dualDoneThisFrame = true;
  g_skipBuildC = 1;
  g_skipBuildD = 1;
  g_skipExecA = 1;
  g_skipExecC = 1;
  g_skipExecD = 1;

  const uint32_t n = ++g_execDualCount;
  if (n <= 8 || (n % 120) == 0) {
    const float dx = rx - lx, dy = ry - ly, dz = rz - lz;
    const float distCm = std::sqrt(dx * dx + dy * dy + dz * dz) * 100.f;
    Log("StereoBuildExec: #%u haveL=%d haveR=%d camDist=%.1fcm sep=%.0fcm", n, g_haveL ? 1 : 0,
        g_haveR ? 1 : 0, distCm, GetStereoSepMeters() * 100.f);
  }
}

// Execute-only dual. Build+Execute from inside ExecA → blackscreen/freeze (do not retry).
void RunExecuteDualFromPhaseA(void* edx) {
  if (!g_origExecA || !g_device || !g_texL || !g_texR)
    return;

  float lx = 0.f, ly = 0.f, lz = 0.f, rx = 0.f, ry = 0.f, rz = 0.f;
  g_inDual.store(true);

  const bool d3dPush = WantsD3dCamPush(GetStereoMode());

  SetStereoEye(StereoEye::Left);
  RefreshLiveCamForStereoEye();
  GetLastStereoCamPos(&lx, &ly, &lz);
  if (d3dPush)
    StereoProjApplyForCurrentEye(g_device);
  RunExecuteEyePass(edx);
  if (CopyCurrentColorToEye(g_device, g_texL))
    g_haveL = true;

  SetStereoEye(StereoEye::Right);
  RefreshLiveCamForStereoEye();
  GetLastStereoCamPos(&rx, &ry, &rz);
  if (d3dPush)
    StereoProjApplyForCurrentEye(g_device);
  RunExecuteEyePass(edx);
  if (CopyCurrentColorToEye(g_device, g_texR))
    g_haveR = true;

  SetStereoEye(StereoEye::Left);
  RefreshLiveCamForStereoEye();
  g_inDual.store(false);
  g_dualDoneThisFrame = true;
  g_skipExecC = 1;
  g_skipExecD = 1;

  const uint32_t n = ++g_execDualCount;
  if (n <= 8 || (n % 120) == 0) {
    const float dx = rx - lx, dy = ry - ly, dz = rz - lz;
    const float distCm = std::sqrt(dx * dx + dy * dy + dz * dz) * 100.f;
    Log("StereoExecDual: #%u haveL=%d haveR=%d camDist=%.1fcm sep=%.0fcm d3d=%d", n,
        g_haveL ? 1 : 0, g_haveR ? 1 : 0, distCm, GetStereoSepMeters() * 100.f, d3dPush ? 1 : 0);
  }
}

void __fastcall HookExecA(void* self, void* edx) {
  if (!g_origExecA)
    return;
  g_lastThisPhaseA = self;
  const StereoMode mode = GetStereoMode();
  if (NeedsExecHooks(mode) && g_skipExecA > 0 && !g_inDual.load()) {
    --g_skipExecA;
    return;
  }
  if (g_inDual.load()) {
    g_origExecA(self, edx);
    return;
  }
  // Mode 10/12: orchestrate from Execute. Mode 11 from Build (disabled/unsafe).
  if (!IsExecuteDualMode(mode)) {
    g_origExecA(self, edx);
    return;
  }
  if (g_dualDoneThisFrame || !IsCamMatrixOverrideEnabled()) {
    g_origExecA(self, edx);
    return;
  }
  if (!g_lastThisDraw || !g_origExecD || !g_texL || !g_texR) {
    g_origExecA(self, edx);
    return;
  }
  RunExecuteDualFromPhaseA(edx);
}

void __fastcall HookExecC(void* self, void* edx) {
  if (!g_origExecC)
    return;
  g_lastThisPhaseC = self;
  if (NeedsExecHooks(GetStereoMode()) && g_skipExecC > 0 && !g_inDual.load()) {
    --g_skipExecC;
    return;
  }
  g_origExecC(self, edx);
}

void __fastcall HookExecD(void* self, void* edx) {
  if (!g_origExecD)
    return;
  g_lastThisDraw = self;
  if (NeedsExecHooks(GetStereoMode()) && g_skipExecD > 0 && !g_inDual.load()) {
    --g_skipExecD;
    return;
  }
  g_origExecD(self, edx);
}

bool SubmitEyeTexture(IDirect3DDevice9* dev, IDirect3DTexture9* tex, vr::EVREye eye,
                      ID3D9VkInteropDevice* interop) {
  if (!dev || !tex || !interop || !vr::VRCompositor())
    return false;

  IDirect3DSurface9* surf = nullptr;
  if (FAILED(tex->GetSurfaceLevel(0, &surf)) || !surf)
    return false;

  ID3D9VkInteropTexture* vtex = nullptr;
  if (FAILED(surf->QueryInterface(__uuidof(ID3D9VkInteropTexture), reinterpret_cast<void**>(&vtex))) ||
      !vtex) {
    surf->Release();
    return false;
  }

  VkImage image = nullptr;
  VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageCreateInfo info{};
  info.sType = 14;
  uint32_t qfiScratch = 0;
  info.queueFamilyIndexCount = 1;
  info.pQueueFamilyIndices = &qfiScratch;
  if (FAILED(vtex->GetVulkanImageInfo(&image, &oldLayout, &info)) || !image) {
    vtex->Release();
    surf->Release();
    return false;
  }

  VkInstance instance = nullptr;
  VkPhysicalDevice phys = nullptr;
  VkDevice vkdev = nullptr;
  interop->GetVulkanHandles(&instance, &phys, &vkdev);
  VkQueue queue = nullptr;
  uint32_t qIndex = 0, qFamily = 0;
  interop->GetSubmissionQueue(&queue, &qIndex, &qFamily);

  vr::VRVulkanTextureData_t vd{};
  vd.m_nImage = reinterpret_cast<uint64_t>(image);
  vd.m_pDevice = reinterpret_cast<VkDevice_T*>(vkdev);
  vd.m_pPhysicalDevice = reinterpret_cast<VkPhysicalDevice_T*>(phys);
  vd.m_pInstance = reinterpret_cast<VkInstance_T*>(instance);
  vd.m_pQueue = reinterpret_cast<VkQueue_T*>(queue);
  vd.m_nQueueFamilyIndex = qFamily;
  vd.m_nWidth = info.extent.width;
  vd.m_nHeight = info.extent.height;
  vd.m_nFormat = static_cast<uint32_t>(info.format);
  vd.m_nSampleCount = info.samples ? static_cast<uint32_t>(info.samples) : 1u;

  vr::Texture_t t{};
  t.handle = &vd;
  t.eType = vr::TextureType_Vulkan;
  t.eColorSpace = vr::ColorSpace_Gamma;

  // TextureBounds only with a real cover-FOV render. Bounds on game FOV = zoom/weird FOV.
  vr::VRTextureBounds_t bounds{};
  const vr::VRTextureBounds_t* boundsPtr = nullptr;
  if (StereoProjShouldInject() && GetEyeSubmitBounds(eye, &bounds))
    boundsPtr = &bounds;

  const vr::EVRCompositorError err =
      vr::VRCompositor()->Submit(eye, &t, boundsPtr, vr::Submit_Default);
  vtex->Release();
  surf->Release();
  return err == vr::VRCompositorError_None;
}

void LogCachedIpdOnce() {
  if (g_loggedIpd)
    return;
  EyeOffset L{}, R{};
  if (!GetCachedEyeOffset(false, &L) || !GetCachedEyeOffset(true, &R))
    return;
  g_loggedIpd = true;
  const float ipdMm = (R.x - L.x) * 1000.f;
  Log("StereoIPD: L=(%.4f,%.4f,%.4f) R=(%.4f,%.4f,%.4f) sepX=%.1fmm", L.x, L.y, L.z, R.x, R.y, R.z,
      ipdMm);
}

uintptr_t FindFnByMid(const char* midAob, int midOff) {
  const uintptr_t mid = FindPattern(nullptr, midAob);
  if (!mid || mid < static_cast<uintptr_t>(midOff))
    return 0;
  return mid - static_cast<uintptr_t>(midOff);
}

uintptr_t FindDrawSceneBuildRenderList() {
  return FindFnByMid("83 BF 38 09 00 00 FF 0F 84 DE 09 00 00 6A 00 6A 0C", 13);
}

uintptr_t FindPhaseABuildRenderList() {
  return FindFnByMid("83 BF 38 09 00 00 FF 0F 84 76 03 00 00 80 3D", 30);
}

uintptr_t FindPhaseCBuildRenderList() {
  return FindFnByMid("83 BF 38 09 00 00 FF 0F 84 77 02 00 00 8D 8F B0 00 00 00", 39);
}

void LogPhaseCall(const char* name, void* self) {
  const uint32_t left = g_phaseLogLeft.load();
  if (left == 0)
    return;
  g_phaseLogLeft.store(left - 1);
  Log("StereoPhase: %s this=%p frameSeq=%u", name, self, g_frameSeq.load());
}

void DumpPhaseVtableOnce(const char* name, void* self, uintptr_t buildFnAddr) {
  static bool s_dumpedA = false, s_dumpedC = false, s_dumpedD = false;
  bool* flag = nullptr;
  if (name[5] == 'A')
    flag = &s_dumpedA;
  else if (name[5] == 'C')
    flag = &s_dumpedC;
  else
    flag = &s_dumpedD;
  // Must check BEFORE any AOB — callers must not scan every frame either.
  if (!self || !flag || *flag)
    return;
  *flag = true;

  void** vt = *reinterpret_cast<void***>(self);
  if (!vt) {
    Log("StereoVtbl: %s this=%p vt=null", name, self);
    return;
  }
  Log("StereoVtbl: %s this=%p vt=%p buildFn=%p", name, self, static_cast<void*>(vt),
      reinterpret_cast<void*>(buildFnAddr));
  int buildIdx = -1;
  for (int i = 0; i < 16; ++i) {
    if (buildFnAddr && vt[i] == reinterpret_cast<void*>(buildFnAddr))
      buildIdx = i;
    Log("StereoVtbl: %s [%2d]=%p", name, i, vt[i]);
  }
  if (buildIdx >= 0) {
    Log("StereoVtbl: %s BuildRenderList = vtable[%d]", name, buildIdx);
    if (buildIdx + 1 < 16)
      Log("StereoVtbl: %s Execute-candidate vtable[%d]=%p", name, buildIdx + 1, vt[buildIdx + 1]);
    if (buildIdx >= 1)
      Log("StereoVtbl: %s prev-candidate vtable[%d]=%p", name, buildIdx - 1, vt[buildIdx - 1]);
  } else {
    Log("StereoVtbl: %s BuildRenderList NOT in first 16 slots", name);
  }
}

bool HavePhasePtrs() {
  return g_lastThisPhaseA && g_lastThisPhaseC && g_lastThisDraw && g_origPhaseA && g_origPhaseC &&
         g_origBuild;
}

void RunPhaseTriplet(void* drawSelf, void* edx) {
  // Order from mode-5 log: PhaseA -> PhaseC -> DrawScene
  if (g_origPhaseA && g_lastThisPhaseA)
    g_origPhaseA(g_lastThisPhaseA, edx);
  if (g_origPhaseC && g_lastThisPhaseC)
    g_origPhaseC(g_lastThisPhaseC, edx);
  if (g_origBuild && drawSelf)
    g_origBuild(drawSelf, edx);
}

void RunSameFrameDualFromDrawScene(void* drawSelf, void* edx, bool useCurrentRt) {
  LogCachedIpdOnce();

  g_inDual.store(true);

  float lx = 0.f, ly = 0.f, lz = 0.f, rx = 0.f, ry = 0.f, rz = 0.f;

  // LEFT: PhaseA/C already ran naturally with Left eye before DrawScene.
  SetStereoEye(StereoEye::Left);
  RefreshLiveCamForStereoEye();
  GetLastStereoCamPos(&lx, &ly, &lz);
  if (useCurrentRt && g_device)
    StereoProjApplyForCurrentEye(g_device);
  g_origBuild(drawSelf, edx);
  if (g_device) {
    const bool ok = useCurrentRt ? CopyCurrentColorToEye(g_device, g_texL)
                                 : CopyBbToEye(g_device, g_texL);
    if (ok)
      g_haveL = true;
  }

  // RIGHT: re-run full triplet with Right separation
  SetStereoEye(StereoEye::Right);
  RefreshLiveCamForStereoEye();
  GetLastStereoCamPos(&rx, &ry, &rz);
  if (useCurrentRt && g_device)
    StereoProjApplyForCurrentEye(g_device);
  RunPhaseTriplet(drawSelf, edx);
  if (g_device) {
    const bool ok = useCurrentRt ? CopyCurrentColorToEye(g_device, g_texR)
                                 : CopyBbToEye(g_device, g_texR);
    if (ok)
      g_haveR = true;
  }

  SetStereoEye(StereoEye::Left);
  RefreshLiveCamForStereoEye();

  g_inDual.store(false);
  g_dualDoneThisFrame = true;

  const uint32_t n = ++g_sameFrameCount;
  if (n <= 8 || (n % 120) == 0) {
    const float dx = rx - lx, dy = ry - ly, dz = rz - lz;
    const float distCm = std::sqrt(dx * dx + dy * dy + dz * dz) * 100.f;
    Log("StereoSameFrame: #%u haveL=%d haveR=%d rt=%d camDist=%.1fcm sepCfg=%.0fcm", n,
        g_haveL ? 1 : 0, g_haveR ? 1 : 0, useCurrentRt ? 1 : 0, distCm,
        GetStereoSepMeters() * 100.f);
  }
}

void __fastcall HookBuildRenderList(void* self, void* edx) {
  if (!g_origBuild)
    return;

  g_lastThisDraw = self;
  const StereoMode mode = GetStereoMode();

  if (NeedsExecHooks(mode)) {
    TryInstallExecFromVtbl(self, 9, &g_execDHooked, &g_origExecD,
                           reinterpret_cast<void*>(&HookExecD), "ExecDrawScene");
    if (IsBuildExecDualMode(mode) && g_skipBuildD > 0 && !g_inDual.load()) {
      --g_skipBuildD;
      return;
    }
    g_origBuild(self, edx);
    return;
  }

  if (mode == StereoMode::PhaseProbe) {
    LogPhaseCall("DrawScene", self);
    g_origBuild(self, edx);
    return;
  }

  if (mode == StereoMode::VtableProbe) {
    static uintptr_t s_buildD = 0;
    if (IsCamMatrixOverrideEnabled()) {
      if (!s_buildD)
        s_buildD = FindDrawSceneBuildRenderList();
      DumpPhaseVtableOnce("DrawScene", self, s_buildD);
    }
    g_origBuild(self, edx);
    return;
  }

  // Mode 6/7: first DrawScene orchestrates L capture + R triplet.
  // Mode 7 = mode 6 + current-RT copy + proj inject during g_inDual only.
  if (IsSameFrameMode(mode) && IsCamMatrixOverrideEnabled()) {
    if (g_inDual.load()) {
      g_origBuild(self, edx);
      return;
    }
    if (g_dualDoneThisFrame) {
      g_origBuild(self, edx);
      return;
    }
    if (!HavePhasePtrs()) {
      g_origBuild(self, edx);
      return;
    }
    if (!g_texL || !g_texR) {
      g_origBuild(self, edx);
      return;
    }
    RunSameFrameDualFromDrawScene(self, edx, UsesRtAndProj(mode));
    g_projWindow.store(false);
    return;
  }

  if (mode == StereoMode::Off || g_inDual.load() || !IsCamMatrixOverrideEnabled()) {
    g_origBuild(self, edx);
    return;
  }

  if (mode == StereoMode::Probe) {
    g_origBuild(self, edx);
    const uint32_t n = ++g_passCount;
    if (n <= 3 || (n % 300) == 0)
      Log("StereoRender: probe BuildRenderList #%u", n);
    return;
  }

  if (IsTemporalStereoMode(mode)) {
    g_origBuild(self, edx);
    return;
  }

  // Modes 2-3
  g_inDual.store(true);
  const bool wantIpd = (mode >= StereoMode::DualIpd);
  SetStereoEye(StereoEye::Left);
  if (wantIpd)
    RefreshLiveCamForStereoEye();
  g_origBuild(self, edx);
  SetStereoEye(StereoEye::Right);
  if (wantIpd)
    RefreshLiveCamForStereoEye();
  g_origBuild(self, edx);
  SetStereoEye(StereoEye::Left);
  if (wantIpd)
    RefreshLiveCamForStereoEye();
  g_inDual.store(false);
  const uint32_t n = ++g_passCount;
  if (n <= 5 || (n % 300) == 0)
    Log("StereoRender: dual BuildRenderList #%u mode=%d", n, static_cast<int>(mode));
}

void __fastcall HookPhaseA(void* self, void* edx) {
  if (!g_origPhaseA)
    return;
  g_lastThisPhaseA = self;
  const StereoMode mode = GetStereoMode();
  if (mode == StereoMode::PhaseProbe)
    LogPhaseCall("PhaseA", self);
  if (mode == StereoMode::VtableProbe && IsCamMatrixOverrideEnabled()) {
    static uintptr_t s_buildA = 0;
    if (!s_buildA)
      s_buildA = FindPhaseABuildRenderList();
    DumpPhaseVtableOnce("PhaseA", self, s_buildA);
  }

  if (NeedsExecHooks(mode)) {
    TryInstallExecFromVtbl(self, 9, &g_execAHooked, &g_origExecA,
                           reinterpret_cast<void*>(&HookExecA), "ExecPhaseA");
    if (IsBuildExecDualMode(mode)) {
      if (g_inDual.load()) {
        g_origPhaseA(self, edx);
        return;
      }
      if (g_dualDoneThisFrame || !IsCamMatrixOverrideEnabled()) {
        g_origPhaseA(self, edx);
        return;
      }
      // Need prior-frame this ptrs + exec hooks
      if (!g_lastThisPhaseC || !g_lastThisDraw || !g_origExecA || !g_origExecD || !g_texL ||
          !g_texR) {
        g_origPhaseA(self, edx);
        return;
      }
      g_lastThisPhaseA = self;
      RunBuildExecDualFromPhaseABuild(edx);
      return;
    }
    g_origPhaseA(self, edx);
    return;
  }

  // Mode 6/7/8: Left IPD for natural first-chain; open proj window for 7/8
  if (IsSameFrameMode(mode) && IsCamMatrixOverrideEnabled() && !g_inDual.load() &&
      !g_dualDoneThisFrame) {
    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    if (UsesRtAndProj(mode)) {
      g_projWindow.store(true);
      if (g_device)
        StereoProjApplyForCurrentEye(g_device);
    }
  } else if (g_inDual.load() && UsesRtAndProj(mode) && g_device) {
    StereoProjApplyForCurrentEye(g_device);
  }
  g_origPhaseA(self, edx);
}

void __fastcall HookPhaseC(void* self, void* edx) {
  if (!g_origPhaseC)
    return;
  g_lastThisPhaseC = self;
  const StereoMode mode = GetStereoMode();
  if (mode == StereoMode::PhaseProbe)
    LogPhaseCall("PhaseC", self);
  if (mode == StereoMode::VtableProbe && IsCamMatrixOverrideEnabled()) {
    static uintptr_t s_buildC = 0;
    if (!s_buildC)
      s_buildC = FindPhaseCBuildRenderList();
    DumpPhaseVtableOnce("PhaseC", self, s_buildC);
  }

  if (NeedsExecHooks(mode)) {
    TryInstallExecFromVtbl(self, 9, &g_execCHooked, &g_origExecC,
                           reinterpret_cast<void*>(&HookExecC), "ExecPhaseC");
    if (IsBuildExecDualMode(mode) && g_skipBuildC > 0 && !g_inDual.load()) {
      --g_skipBuildC;
      return;
    }
    g_origPhaseC(self, edx);
    return;
  }

  if (IsSameFrameMode(mode) && IsCamMatrixOverrideEnabled() && !g_inDual.load() &&
      !g_dualDoneThisFrame) {
    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
  }
  g_origPhaseC(self, edx);
}

bool HookOneBuild(const char* label, uintptr_t addr, void* detour, BuildRenderList_t* origOut) {
  if (!addr) {
    Log("StereoRender: %s MISS", label);
    return false;
  }
  if (MH_CreateHook(reinterpret_cast<void*>(addr), detour, reinterpret_cast<void**>(origOut)) !=
          MH_OK ||
      MH_EnableHook(reinterpret_cast<void*>(addr)) != MH_OK) {
    Log("StereoRender: %s MH FAIL @ %p", label, reinterpret_cast<void*>(addr));
    *origOut = nullptr;
    return false;
  }
  Log("StereoRender: hooked %s @ %p", label, reinterpret_cast<void*>(addr));
  return true;
}

void TemporalCaptureThisFrame(IDirect3DDevice9* device) {
  if (!device || !g_texL || !g_texR)
    return;
  LogCachedIpdOnce();
  const StereoEye eye = GetStereoEye();
  // Prefer current color RT; BB fallback.
  if (eye == StereoEye::Left) {
    if (CopyCurrentColorToEye(device, g_texL))
      g_haveL = true;
    SetStereoEye(StereoEye::Right);
  } else {
    if (CopyCurrentColorToEye(device, g_texR))
      g_haveR = true;
    SetStereoEye(StereoEye::Left);
  }
  RefreshLiveCamForStereoEye();  // next frame uses new eye immediately in CopyMat
  const uint32_t n = ++g_temporalFrames;
  if (n <= 6 || (n % 120) == 0)
    Log("StereoTemporal: frame #%u captured=%s haveL=%d haveR=%d sep=%.0fcm", n,
        eye == StereoEye::Left ? "L" : "R", g_haveL ? 1 : 0, g_haveR ? 1 : 0,
        GetStereoSepMeters() * 100.f);
}

bool InstallAllThreePhases() {
  bool ok = true;
  ok &= HookOneBuild("DrawScene", FindDrawSceneBuildRenderList(),
                     reinterpret_cast<void*>(&HookBuildRenderList), &g_origBuild);
  ok &= HookOneBuild("PhaseA", FindPhaseABuildRenderList(), reinterpret_cast<void*>(&HookPhaseA),
                     &g_origPhaseA);
  ok &= HookOneBuild("PhaseC", FindPhaseCBuildRenderList(), reinterpret_cast<void*>(&HookPhaseC),
                     &g_origPhaseC);
  return ok;
}

}  // namespace

bool InstallStereoRenderHooks() {
  if (g_ok.load())
    return true;

  ReloadStereoMode();
  const StereoMode mode = GetStereoMode();
  if (mode == StereoMode::Off) {
    Log("StereoRender: mode 0 - skipped");
    return false;
  }

  if (IsTemporalStereoMode(mode)) {
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_loggedIpd = false;
    g_ok = true;
    if (mode == StereoMode::StereoFusion)
      Log("StereoRender: mode 13 StereoFusion (L4D2/BotW: temporal + coverFOV + TextureBounds)");
    else
      Log("StereoRender: mode 4 temporal IPD");
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 0 if freeze");
    return true;
  }

  if (mode == StereoMode::PhaseProbe || mode == StereoMode::VtableProbe ||
      mode == StereoMode::ExecuteDual || mode == StereoMode::BuildExecDual ||
      mode == StereoMode::D3dCamDual) {
    g_phaseLogLeft.store(40);
    g_frameSeq.store(0);
    g_dualDoneThisFrame = false;
    g_skipExecA = g_skipExecC = g_skipExecD = 0;
    g_skipBuildC = g_skipBuildD = 0;
    g_haveL = g_haveR = false;
    g_execAHooked = g_execCHooked = g_execDHooked = false;
    g_origExecA = g_origExecC = g_origExecD = nullptr;
    g_execDualCount.store(0);
    SetStereoEye(StereoEye::Left);
    g_ok = InstallAllThreePhases();
    if (mode == StereoMode::BuildExecDual)
      Log("StereoRender: mode 11 Build+Exec dual — UNSAFE/blackscreen", g_ok.load() ? 1 : 0);
    else if (mode == StereoMode::D3dCamDual)
      Log("StereoRender: mode 12 Exec dual + multi-cam + D3D VIEW ok=%d", g_ok.load() ? 1 : 0);
    else if (mode == StereoMode::ExecuteDual)
      Log("StereoRender: mode 10 EXECUTE dual (vt[9]) ok=%d", g_ok.load() ? 1 : 0);
    else if (mode == StereoMode::VtableProbe)
      Log("StereoRender: mode 9 VTABLE probe (find Execute) ok=%d", g_ok.load() ? 1 : 0);
    else
      Log("StereoRender: mode 5 phase probe ok=%d", g_ok.load() ? 1 : 0);
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 0 if freeze");
    return g_ok.load();
  }

  if (mode == StereoMode::SameFrameDual || mode == StereoMode::GBufferRtDual ||
      mode == StereoMode::FusionSwap) {
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_loggedIpd = false;
    g_loggedRt = false;
    g_dualDoneThisFrame = false;
    g_projWindow.store(false);
    g_sameFrameCount.store(0);
    g_ok = InstallAllThreePhases();
    if (mode == StereoMode::FusionSwap) {
      Log("StereoRender: mode 8 = mode7 + Submit eye-swap ok=%d", g_ok.load() ? 1 : 0);
    } else if (mode == StereoMode::GBufferRtDual) {
      Log("StereoRender: mode 7 RT-copy + VS/proj window ok=%d", g_ok.load() ? 1 : 0);
    } else {
      Log("StereoRender: mode 6 SAME-FRAME dual (PhaseA->C->Draw L then R) ok=%d",
          g_ok.load() ? 1 : 0);
    }
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 0 if freeze");
    return g_ok.load();
  }

  const uintptr_t addr = FindDrawSceneBuildRenderList();
  if (!HookOneBuild("DrawScene", addr, reinterpret_cast<void*>(&HookBuildRenderList), &g_origBuild))
    return false;
  g_ok = true;
  Log("StereoRender: mode=%d DrawScene ready", static_cast<int>(mode));
  Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 0 if freeze");
  return true;
}

void StereoRenderOnDevice(IDirect3DDevice9* device) {
  if (!device)
    return;
  g_device = device;
  g_frameSeq.fetch_add(1);

  const StereoMode mode = GetStereoMode();

  if (mode == StereoMode::PhaseProbe) {
    static uint32_t s_end = 0;
    if (s_end < 8 || (s_end % 300) == 0)
      Log("StereoPhase: EndScene/Submit boundary frameSeq=%u", g_frameSeq.load());
    ++s_end;
    return;
  }

  if (IsSameFrameMode(mode) || IsExecuteDualMode(mode) || IsBuildExecDualMode(mode)) {
    g_dualDoneThisFrame = false;
    g_skipExecA = g_skipExecC = g_skipExecD = 0;
    g_skipBuildC = g_skipBuildD = 0;
    g_projWindow.store(false);

    if (!IsCamMatrixOverrideEnabled())
      return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
      return;
    D3DSURFACE_DESC desc{};
    bb->GetDesc(&desc);
    bb->Release();
    EnsureEyeRts(device, desc.Width, desc.Height);
    return;
  }

  if (!IsTemporalStereoMode(mode))
    return;
  if (!IsCamMatrixOverrideEnabled())
    return;

  IDirect3DSurface9* bb = nullptr;
  if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    return;
  D3DSURFACE_DESC desc{};
  bb->GetDesc(&desc);
  bb->Release();
  if (!EnsureEyeRts(device, desc.Width, desc.Height))
    return;
  TemporalCaptureThisFrame(device);
}

bool StereoTrySubmitEyes(IDirect3DDevice9* device, ID3D9VkInteropDevice* interop) {
  const StereoMode mode = GetStereoMode();
  if (!IsTemporalStereoMode(mode) && mode != StereoMode::SameFrameDual &&
      mode != StereoMode::GBufferRtDual && mode != StereoMode::FusionSwap &&
      mode != StereoMode::ExecuteDual && mode != StereoMode::BuildExecDual &&
      mode != StereoMode::D3dCamDual)
    return false;
  if (!device || !interop || !g_texL || !g_texR)
    return false;
  if (!IsCamMatrixOverrideEnabled())
    return false;
  if (!g_haveL || !g_haveR)
    return false;

  interop->FlushRenderingCommands();
  interop->LockSubmissionQueue();
  bool okL = false, okR = false;
  if (mode == StereoMode::FusionSwap) {
    // Diagnostic: feed captured-Left texture to Right eye and vice versa.
    okL = SubmitEyeTexture(device, g_texR, vr::Eye_Left, interop);
    okR = SubmitEyeTexture(device, g_texL, vr::Eye_Right, interop);
  } else {
    okL = SubmitEyeTexture(device, g_texL, vr::Eye_Left, interop);
    okR = SubmitEyeTexture(device, g_texR, vr::Eye_Right, interop);
  }
  interop->ReleaseSubmissionQueue();

  static uint32_t s_n = 0;
  if ((++s_n) <= 8 || (s_n % 120) == 0)
    Log("StereoSubmit: L=%d R=%d mode=%d swap=%d", okL ? 1 : 0, okR ? 1 : 0,
        static_cast<int>(mode), mode == StereoMode::FusionSwap ? 1 : 0);
  return okL && okR;
}

bool StereoInDualPass() {
  return g_inDual.load();
}

bool StereoProjWindowActive() {
  return g_projWindow.load() || g_inDual.load();
}

}  // namespace asi
