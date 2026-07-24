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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <intrin.h>  // _ReturnAddress for mode 18 render-root probe

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

// Mode 14: canvas RT sized so the game image sits inside the full HMD frustum.
// Cap keeps two ARGB8 RTs small enough for the 32-bit address space.
constexpr uint32_t kCanvasMaxDim = 2048;

void ComputeCanvasSize(uint32_t bbW, uint32_t bbH, uint32_t* outW, uint32_t* outH) {
  *outW = bbW;
  *outH = bbH;
  float coverH = 0.f, coverV = 0.f;
  if (!GetCoverFovTangents(&coverH, &coverV))
    return;
  float gameH = 0.f, gameV = 0.f;
  GetGameFovTangents(&gameH, &gameV);
  if (!(gameH > 0.05f) || !(gameV > 0.05f))
    return;

  float sx = coverH / gameH;
  float sy = coverV / gameV;
  sx = (std::max)(1.f, (std::min)(sx, 4.f));
  sy = (std::max)(1.f, (std::min)(sy, 4.f));

  uint32_t w = static_cast<uint32_t>(bbW * sx + 0.5f);
  uint32_t h = static_cast<uint32_t>(bbH * sy + 0.5f);
  if (w > kCanvasMaxDim)
    w = kCanvasMaxDim;
  if (h > kCanvasMaxDim)
    h = kCanvasMaxDim;
  *outW = w;
  *outH = h;
}

// Mode 14: clear canvas to black, then StretchRect the game backbuffer to the
// rectangle where its FOV really lives inside THIS eye's asymmetric HMD frustum.
// Submit later uses nullptr bounds — the canvas spans the full frustum, so the
// angular mapping is correct per eye (this is what nullptr-bounds-on-BB broke).
bool CopySurfToEyeCanvas(IDirect3DDevice9* dev, IDirect3DSurface9* bb, IDirect3DTexture9* tex,
                         vr::EVREye eye) {
  if (!dev || !bb || !tex)
    return false;

  float l = 0.f, r = 0.f, t = 0.f, b = 0.f;
  if (!GetEyeRawProjection(eye, &l, &r, &t, &b) || !(r > l) || !(b > t))
    return CopyBbToEye(dev, tex);
  float gameH = 0.f, gameV = 0.f;
  GetGameFovTangents(&gameH, &gameV);
  if (!(gameH > 0.05f) || !(gameV > 0.05f))
    return CopyBbToEye(dev, tex);

  bb->AddRef();
  IDirect3DSurface9* dst = nullptr;
  if (FAILED(tex->GetSurfaceLevel(0, &dst)) || !dst) {
    bb->Release();
    return false;
  }

  D3DSURFACE_DESC dd{};
  dst->GetDesc(&dd);
  D3DSURFACE_DESC sd{};
  bb->GetDesc(&sd);
  const float W = static_cast<float>(dd.Width);
  const float H = static_cast<float>(dd.Height);
  const float SW = static_cast<float>(sd.Width);
  const float SH = static_cast<float>(sd.Height);

  // Map the OVERLAP of game frustum [-gameH..+gameH]x[-gameV..+gameV] and eye
  // frustum [l..r]x[t..b] (tangent space, top of texture = negative tan).
  // Where the game renders MORE than the eye sees (FOV patch), crop the SOURCE;
  // where it renders less, inset the DEST. Clamping only the dest (old code)
  // squeezed the image differently per eye → objects jumped left/right.
  const float txLo = (std::max)(-gameH, l);
  const float txHi = (std::min)(gameH, r);
  const float tyLo = (std::max)(-gameV, t);
  const float tyHi = (std::min)(gameV, b);
  if (txHi - txLo < 0.05f || tyHi - tyLo < 0.05f) {
    dst->Release();
    bb->Release();
    return CopyBbToEye(dev, tex);
  }

  RECT src{};
  src.left = static_cast<LONG>(SW * (txLo + gameH) / (2.f * gameH));
  src.right = static_cast<LONG>(SW * (txHi + gameH) / (2.f * gameH));
  src.top = static_cast<LONG>(SH * (tyLo + gameV) / (2.f * gameV));
  src.bottom = static_cast<LONG>(SH * (tyHi + gameV) / (2.f * gameV));
  RECT rc{};
  rc.left = static_cast<LONG>(W * (txLo - l) / (r - l));
  rc.right = static_cast<LONG>(W * (txHi - l) / (r - l));
  rc.top = static_cast<LONG>(H * (tyLo - t) / (b - t));
  rc.bottom = static_cast<LONG>(H * (tyHi - t) / (b - t));

  auto clampRect = [](RECT* rr, LONG w, LONG h) {
    if (rr->left < 0)
      rr->left = 0;
    if (rr->top < 0)
      rr->top = 0;
    if (rr->right > w)
      rr->right = w;
    if (rr->bottom > h)
      rr->bottom = h;
  };
  clampRect(&src, static_cast<LONG>(sd.Width), static_cast<LONG>(sd.Height));
  clampRect(&rc, static_cast<LONG>(dd.Width), static_cast<LONG>(dd.Height));

  bool ok = false;
  if (rc.right - rc.left >= 16 && rc.bottom - rc.top >= 16 && src.right - src.left >= 16 &&
      src.bottom - src.top >= 16) {
    dev->ColorFill(dst, nullptr, D3DCOLOR_XRGB(0, 0, 0));
    ok = SUCCEEDED(dev->StretchRect(bb, &src, dst, &rc, D3DTEXF_LINEAR));
  }
  dst->Release();
  bb->Release();
  if (!ok) {
    static bool s_warned = false;
    if (!s_warned) {
      s_warned = true;
      Log("StereoCanvas: sub-rect StretchRect FAILED — falling back to full stretch "
          "(geometry will be wrong; report this log line)");
    }
    return CopyBbToEye(dev, tex);
  }

  static bool s_loggedL = false, s_loggedR = false;
  bool* logged = (eye == vr::Eye_Right) ? &s_loggedR : &s_loggedL;
  if (!*logged) {
    *logged = true;
    Log("StereoCanvas: %s canvas=%ux%u dst=(%ld,%ld)-(%ld,%ld) src=(%ld,%ld)-(%ld,%ld) "
        "gameTan=(%.3f,%.3f) eyeTan=(%.3f..%.3f, %.3f..%.3f)",
        eye == vr::Eye_Right ? "R" : "L", dd.Width, dd.Height, rc.left, rc.top, rc.right,
        rc.bottom, src.left, src.top, src.right, src.bottom, gameH, gameV, l, r, t, b);
  }
  return true;
}

bool CopyBbToEyeCanvas(IDirect3DDevice9* dev, IDirect3DTexture9* tex, vr::EVREye eye) {
  if (!dev || !tex)
    return false;
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    return false;
  const bool ok = CopySurfToEyeCanvas(dev, bb, tex, eye);
  bb->Release();
  return ok;
}

// Mode 23: the scene may still live in the CURRENT color RT (offscreen) at
// per-phase exec time; prefer it, fall back to the backbuffer.
bool CopyCurrentRtToEyeCanvas(IDirect3DDevice9* dev, IDirect3DTexture9* tex, vr::EVREye eye) {
  if (!dev || !tex)
    return false;
  IDirect3DSurface9* src = nullptr;
  if (FAILED(dev->GetRenderTarget(0, &src)) || !src)
    return CopyBbToEyeCanvas(dev, tex, eye);
  const bool ok = CopySurfToEyeCanvas(dev, src, tex, eye);
  src->Release();
  if (ok)
    return true;
  return CopyBbToEyeCanvas(dev, tex, eye);
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

bool IsExecViewPhaseMode(StereoMode mode) {
  return mode == StereoMode::ExecViewPhaseDual || mode == StereoMode::ExecViewConstDual;
}

bool NeedsExecHooks(StereoMode mode) {
  return IsExecuteDualMode(mode) || IsBuildExecDualMode(mode) ||
         mode == StereoMode::RenderRootProbe || IsExecViewPhaseMode(mode);
}

// ---- Mode 18: render-root probe (READ-ONLY) ---------------------------------
// Log unique return addresses of the Build/Execute phase hooks. The common
// caller is the Rage render-root ("RenderView" equivalent) we need to hook for
// a true same-frame dual render (kills the temporal L/R jumping for good).
struct RetSeen {
  const char* tag;
  void* ret;
  uint32_t count;
};
RetSeen g_retSeen[64]{};

// ---- Mode 19: root dispatch probe (passthrough hooks + per-frame counters) ---
// Roots found by mode 18/19 (GTA IV CE 1.2.0.59, PlayGTAIV.exe, base 0x400000).
// All are thiscall with NO stack args (plain C3 ret) -> BuildRenderList_t works.
// [3] FrameA 0x5C2380: contains the ExecRoot call (+ much more, len 0x866).
// [4] BuildWrapB 0x5C2BF0: mov ecx,0x118D7F0; call BuildRootA (tiny wrapper).
// Their common caller should contain the build->exec list SWAP that the failed
// mode-20 v2 in-frame exec re-call skipped (crash 2026-07-24).
constexpr int kNumRoots = 5;
constexpr uint32_t kRootRva[kNumRoots] = {0x4F73B0, 0x4F7F00, 0x687680, 0x1C2380, 0x1C2BF0};
const char* const kRootName[kNumRoots] = {"ExecRoot", "BuildRootA", "BuildExecCD", "FrameA",
                                          "BuildWrapB"};
const uint8_t kRootSig0[] = {0x53, 0x55, 0x56, 0x57, 0x8B, 0xF1};
const uint8_t kRootSig1[] = {0x56, 0x57, 0x8B, 0xF9};
const uint8_t kRootSig2[] = {0x83, 0xEC, 0x20, 0x53, 0x56, 0x57, 0x8B, 0xF9};
const uint8_t kRootSig3[] = {0x83, 0xEC, 0x08, 0x56, 0xE8};
const uint8_t kRootSig4[] = {0xB9, 0xF0, 0xD7, 0x18, 0x01, 0xE8};
BuildRenderList_t g_origRoot[kNumRoots] = {};
std::atomic<uint32_t> g_rootCount[kNumRoots]{};
char g_rootOrder[96];
std::atomic<uint32_t> g_rootOrderLen{0};

void NoteRoot(int i) {
  ++g_rootCount[i];
  const uint32_t p = g_rootOrderLen.fetch_add(1);
  if (p < sizeof(g_rootOrder) - 1)
    g_rootOrder[p] = static_cast<char>('0' + i);
}

void LogRenderRootRet(const char* tag, void* ret);

void* g_root0Self = nullptr;
void* g_root3Self = nullptr;

// ---- Mode 21: view-matrix scan (exec side, READ-ONLY) ------------------------
// Rage stores the per-frame camera for the render side somewhere between the
// build (main thread) and the exec (render thread). Scan the render-manager
// object window and the .data section for the CURRENT cam pos / view translation.
int ScanRangeForTriple(const char* tag, uintptr_t begin, uintptr_t end, float x, float y,
                       float z, int budget) {
  const float eps = 0.05f;
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  int found = 0;
  for (uintptr_t p = begin; p + 12 <= end && found < budget; p += 4) {
    const float* f = reinterpret_cast<const float*>(p);
    if (f[0] > x - eps && f[0] < x + eps && f[1] > y - eps && f[1] < y + eps &&
        f[2] > z - eps && f[2] < z + eps) {
      Log("ViewScan: %s hit @ %p (exeRva 0x%X)", tag, reinterpret_cast<void*>(p),
          static_cast<unsigned>(p - base));
      ++found;
    }
  }
  return found;
}

void RunViewMatrixScanGuarded() {
  __try {
    float px = 0.f, py = 0.f, pz = 0.f;
    if (!GetLastStereoCamPos(&px, &py, &pz))
      return;
    float vm[16];
    const bool haveView = BuildLiveViewMatrix16(vm);
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const uintptr_t mgr = base + 0xD8D7F0;  // 0x118D7F0 at preferred base
    Log("ViewScan: scanning for pos=(%.2f,%.2f,%.2f) viewT=(%.2f,%.2f,%.2f)", px, py, pz,
        haveView ? vm[12] : 0.f, haveView ? vm[13] : 0.f, haveView ? vm[14] : 0.f);
    ScanRangeForTriple("mgrPOS", mgr, mgr + 0x8000, px, py, pz, 12);
    ScanRangeForTriple("dataPOS", base + 0xC30000, base + 0xC30000 + 0x124200, px, py, pz, 12);
    if (haveView) {
      ScanRangeForTriple("mgrVIEWT", mgr, mgr + 0x8000, vm[12], vm[13], vm[14], 12);
      ScanRangeForTriple("dataVIEWT", base + 0xC30000, base + 0xC30000 + 0x124200, vm[12],
                         vm[13], vm[14], 12);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Log("ViewScan: EXCEPTION during scan (ignored)");
  }
}

// ---- Mode 22: exec-view dual (render thread) ---------------------------------
// The manager holds two live camera WORLD matrices (found by mode 21). The view
// matrix is derived from them at EXEC time, so shifting their position row by
// the eye delta between two ExecRoot passes renders the SAME lists from two eye
// positions — true same-frame stereo without touching the build thread.
constexpr uint32_t kMgrCamPosRvaA = 0xD8D8C0;  // pos row of cam matrix A
constexpr uint32_t kMgrCamPosRvaB = 0xD8D9C0;  // pos row of cam matrix B
std::atomic<bool> g_execDualDead{false};
std::atomic<uint32_t> g_execViewDualCount{0};
std::atomic<uint32_t> g_execViewSkips{0};
// Crash forensics: which step of the dual pass threw, and with what code.
volatile LONG g_execViewStage = 0;
volatile DWORD g_execViewExcCode = 0;
volatile uintptr_t g_execViewExcAddr = 0;

DWORD ExecViewFilter(EXCEPTION_POINTERS* ep) {
  g_execViewExcCode = ep->ExceptionRecord->ExceptionCode;
  g_execViewExcAddr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
  return EXCEPTION_EXECUTE_HANDLER;
}

bool RunExecViewDualGuarded(void* self, void* edx) {
  __try {
    g_execViewStage = 1;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    float* posA = reinterpret_cast<float*>(base + kMgrCamPosRvaA);
    float* posB = reinterpret_cast<float*>(base + kMgrCamPosRvaB);
    float dx = 0.f, dy = 0.f, dz = 0.f;
    float cx = 0.f, cy = 0.f, cz = 0.f;
    // Sanity: only dual-render when the manager matrices really hold our cam
    // (scripted/cutscene cams bypass our CopyMat hooks — then stay native mono).
    const bool sane = GetStereoEyeRightDeltaWorld(&dx, &dy, &dz) &&
                      GetLastStereoCamPos(&cx, &cy, &cz) && std::fabs(posA[0] - cx) < 2.f &&
                      std::fabs(posA[1] - cy) < 2.f && std::fabs(posA[2] - cz) < 2.f;
    if (!sane) {
      ++g_execViewSkips;
      g_origRoot[0](self, edx);
      return true;
    }
    // Pass 1 = LEFT (update thread keeps eye Left in the CCam -> snapshot is L).
    g_execViewStage = 2;  // exec pass 1
    g_origRoot[0](self, edx);
    g_execViewStage = 3;  // capture L
    if (CopyBbToEyeCanvas(g_device, g_texL, vr::Eye_Left))
      g_haveL = true;
    // Shift both matrices to the RIGHT eye and draw again.
    g_execViewStage = 4;  // matrix shift
    const float ax = posA[0], ay = posA[1], az = posA[2];
    const float bx = posB[0], by = posB[1], bz = posB[2];
    posA[0] = ax + dx;
    posA[1] = ay + dy;
    posA[2] = az + dz;
    posB[0] = bx + dx;
    posB[1] = by + dy;
    posB[2] = bz + dz;
    g_execViewStage = 5;  // exec pass 2
    g_origRoot[0](self, edx);
    g_execViewStage = 6;  // capture R
    if (CopyBbToEyeCanvas(g_device, g_texR, vr::Eye_Right))
      g_haveR = true;
    g_execViewStage = 7;  // restore
    posA[0] = ax;
    posA[1] = ay;
    posA[2] = az;
    posB[0] = bx;
    posB[1] = by;
    posB[2] = bz;
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    return false;
  }
}

void __fastcall HookRoot0(void* self, void* edx) {
  if (!g_root0Self)
    Log("StereoRootDual: ExecRoot first native call self=%p threadId=%u", self,
        GetCurrentThreadId());
  g_root0Self = self;
  const StereoMode mode = GetStereoMode();
  if (mode == StereoMode::RootDispatchProbe) {
    NoteRoot(0);
    LogRenderRootRet("callerOfExecRoot", _ReturnAddress());
  }
  if (mode == StereoMode::ViewMatrixScan && IsCamMatrixOverrideEnabled()) {
    static uint32_t s_n = 0;
    if ((s_n % 300) == 0)
      RunViewMatrixScanGuarded();
    ++s_n;
  }
  if (!g_origRoot[0])
    return;

  if (mode == StereoMode::ExecViewDual && !g_execDualDead.load() && !g_inDual.load() &&
      IsCamMatrixOverrideEnabled() && g_device && g_texL && g_texR) {
    g_inDual.store(true);
    const bool ok = RunExecViewDualGuarded(self, edx);
    g_inDual.store(false);
    if (!ok) {
      g_execDualDead.store(true);
      g_haveL = g_haveR = false;
      const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
      Log("StereoExecView: EXCEPTION stage=%ld code=0x%08X addr=%p (exeRva 0x%X) - "
          "PERMANENTLY disabled, native mono continues",
          g_execViewStage, g_execViewExcCode, reinterpret_cast<void*>(g_execViewExcAddr),
          static_cast<unsigned>(g_execViewExcAddr - base));
      g_origRoot[0](self, edx);
      return;
    }
    const uint32_t n = ++g_execViewDualCount;
    if (n <= 6 || (n % 300) == 0)
      Log("StereoExecView: #%u haveL=%d haveR=%d skips=%u sep=%.0fcm", n, g_haveL ? 1 : 0,
          g_haveR ? 1 : 0, g_execViewSkips.load(), GetStereoSepMeters() * 100.f);
    return;
  }

  g_origRoot[0](self, edx);
}
std::atomic<uint32_t> g_rootDualCount{0};
void LogCachedIpdOnce();

// Mode 20 v2: BUILD + EXEC per eye, both in one frame. v1 (build-only dual)
// proved Rage double-buffers: BuildRootA only builds draw lists; ExecRoot draws
// them (next frame). Dual build alone = second build overwrites the first, one
// mono image, no parallax, barely any FPS cost (2026-07-24 headset test).
// v2 CRASHED in the exec re-call (2026-07-24): the native frame does a list
// SWAP between build and exec that we skip. SEH guard now converts a faulting
// dual into a one-time fallback to native rendering instead of a process kill.
std::atomic<bool> g_rootDualDead{false};

// v3: replicate the NATIVE frame-start sequence per eye: FrameA (render setup)
// BEFORE ExecRoot. v2 called ExecRoot bare and crashed instantly.
bool RunRootDualGuarded(void* self, void* edx) {
  __try {
    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    g_origRoot[1](self, edx);             // build L lists
    g_origRoot[3](g_root3Self, nullptr);  // FrameA render setup
    g_origRoot[0](g_root0Self, nullptr);  // draw L lists now
    if (CopyBbToEyeCanvas(g_device, g_texL, vr::Eye_Left))
      g_haveL = true;
    SetStereoEye(StereoEye::Right);
    RefreshLiveCamForStereoEye();
    g_origRoot[1](self, edx);             // build R lists
    g_origRoot[3](g_root3Self, nullptr);  // FrameA render setup
    g_origRoot[0](g_root0Self, nullptr);  // draw R lists now
    if (CopyBbToEyeCanvas(g_device, g_texR, vr::Eye_Right))
      g_haveR = true;
    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void __fastcall HookRoot1(void* self, void* edx) {
  const StereoMode mode = GetStereoMode();
  if (mode == StereoMode::RootDispatchProbe) {
    NoteRoot(1);
    LogRenderRootRet("callerOfBuildRootA", _ReturnAddress());
  }
  if (mode == StereoMode::ViewMatrixScan) {
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      Log("ViewScan: BuildRootA threadId=%u (compare with ExecRoot line)",
          GetCurrentThreadId());
    }
  }
  if (!g_origRoot[1])
    return;

  if (mode == StereoMode::SameFrameRootDual && !g_rootDualDead.load() && !g_inDual.load() &&
      IsCamMatrixOverrideEnabled() && g_device && g_texL && g_texR && g_origRoot[0] &&
      g_root0Self && g_origRoot[3] && g_root3Self) {
    g_inDual.store(true);
    LogCachedIpdOnce();
    const bool ok = RunRootDualGuarded(self, edx);
    g_inDual.store(false);
    if (!ok) {
      g_rootDualDead.store(true);
      g_haveL = g_haveR = false;
      Log("StereoRootDual: EXCEPTION in dual pass - PERMANENTLY disabled, native "
          "rendering continues (mono)");
      g_origRoot[1](self, edx);  // make sure this frame still gets built
      return;
    }
    const uint32_t n = ++g_rootDualCount;
    if (n <= 6 || (n % 300) == 0)
      Log("StereoRootDual: #%u v2 build+exec haveL=%d haveR=%d sep=%.0fcm", n, g_haveL ? 1 : 0,
          g_haveR ? 1 : 0, GetStereoSepMeters() * 100.f);
    return;
  }

  g_origRoot[1](self, edx);
}
void __fastcall HookRoot2(void* self, void* edx) {
  if (GetStereoMode() == StereoMode::RootDispatchProbe) {
    NoteRoot(2);
    LogRenderRootRet("callerOfBuildExecCD", _ReturnAddress());
  }
  if (g_origRoot[2])
    g_origRoot[2](self, edx);
}

void __fastcall HookRoot3(void* self, void* edx) {
  g_root3Self = self;
  if (GetStereoMode() == StereoMode::RootDispatchProbe) {
    NoteRoot(3);
    LogRenderRootRet("callerOfFrameA", _ReturnAddress());
  }
  if (g_origRoot[3])
    g_origRoot[3](self, edx);
}

void __fastcall HookRoot4(void* self, void* edx) {
  if (GetStereoMode() == StereoMode::RootDispatchProbe) {
    NoteRoot(4);
    LogRenderRootRet("callerOfBuildWrapB", _ReturnAddress());
  }
  if (g_origRoot[4])
    g_origRoot[4](self, edx);
}

bool RootSigMatches(uintptr_t addr, const uint8_t* sig, size_t n) {
  return std::memcmp(reinterpret_cast<const void*>(addr), sig, n) == 0;
}

bool HookOneBuild(const char* label, uintptr_t addr, void* detour, BuildRenderList_t* origOut);

// nRoots = 3 for mode 20 (dual needs only ExecRoot/BuildRootA), 5 for mode 19.
bool InstallRootProbeHooks(int nRoots) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uint8_t* sigs[kNumRoots] = {kRootSig0, kRootSig1, kRootSig2, kRootSig3, kRootSig4};
  const size_t sigLens[kNumRoots] = {sizeof(kRootSig0), sizeof(kRootSig1), sizeof(kRootSig2),
                                     sizeof(kRootSig3), sizeof(kRootSig4)};
  void* detours[kNumRoots] = {
      reinterpret_cast<void*>(&HookRoot0), reinterpret_cast<void*>(&HookRoot1),
      reinterpret_cast<void*>(&HookRoot2), reinterpret_cast<void*>(&HookRoot3),
      reinterpret_cast<void*>(&HookRoot4)};
  bool ok = true;
  for (int i = 0; i < nRoots; ++i) {
    const uintptr_t addr = base + kRootRva[i];
    if (!RootSigMatches(addr, sigs[i], sigLens[i])) {
      Log("RootProbe: %s prologue MISMATCH @ %p - exe version differs, skip", kRootName[i],
          reinterpret_cast<void*>(addr));
      ok = false;
      continue;
    }
    ok &= HookOneBuild(kRootName[i], addr, detours[i], &g_origRoot[i]);
  }
  return ok;
}

void LogRenderRootRet(const char* tag, void* ret) {
  for (auto& e : g_retSeen) {
    if (e.tag == tag && e.ret == ret) {
      ++e.count;
      if ((e.count % 1200) == 0)
        Log("RenderRoot: %s ret=%p count=%u", tag, ret, e.count);
      return;
    }
    if (!e.tag) {
      e.tag = tag;
      e.ret = ret;
      e.count = 1;
      const uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
      HMODULE owner = nullptr;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(ret), &owner);
      char name[MAX_PATH]{};
      if (owner)
        GetModuleFileNameA(owner, name, MAX_PATH);
      const char* slash = std::strrchr(name, '\\');
      Log("RenderRoot: NEW %s ret=%p rvaExe=0x%X modBase=%p module=%s", tag, ret,
          static_cast<unsigned>(reinterpret_cast<uintptr_t>(ret) - exeBase),
          static_cast<void*>(owner), slash ? slash + 1 : name);
      return;
    }
  }
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

// Mode 24: patch params for the SetVertexShaderConstantF view-translate hook
// (stereo_proj.cpp). Written and read on the render thread only.
std::atomic<bool> g_vsPatchOn{false};
float g_vsPatchCam[3] = {};
float g_vsPatchDelta[3] = {};

// Mode 23: per-phase double exec (proven crash-free by mode 10) + manager-cam
// matrix shift between the passes (the piece mode 10 was missing). Runs inside
// the first HookExecA of the frame on the render thread.
bool RunExecViewPhaseDualGuarded(void* edx) {
  __try {
    g_execViewStage = 1;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    float* posA = reinterpret_cast<float*>(base + kMgrCamPosRvaA);
    float* posB = reinterpret_cast<float*>(base + kMgrCamPosRvaB);
    float dx = 0.f, dy = 0.f, dz = 0.f;
    float cx = 0.f, cy = 0.f, cz = 0.f;
    const bool sane = GetStereoEyeRightDeltaWorld(&dx, &dy, &dz) &&
                      GetLastStereoCamPos(&cx, &cy, &cz) && std::fabs(posA[0] - cx) < 2.f &&
                      std::fabs(posA[1] - cy) < 2.f && std::fabs(posA[2] - cz) < 2.f;
    if (!sane) {
      ++g_execViewSkips;
      RunExecuteEyePass(edx);
      return true;
    }
    g_execViewStage = 2;  // exec pass 1 (LEFT — CCam snapshot is the left eye)
    RunExecuteEyePass(edx);
    g_execViewStage = 3;  // capture L
    if (CopyCurrentRtToEyeCanvas(g_device, g_texL, vr::Eye_Left))
      g_haveL = true;
    g_execViewStage = 4;  // matrix shift to RIGHT eye
    const float ax = posA[0], ay = posA[1], az = posA[2];
    const float bx = posB[0], by = posB[1], bz = posB[2];
    posA[0] = ax + dx;
    posA[1] = ay + dy;
    posA[2] = az + dz;
    posB[0] = bx + dx;
    posB[1] = by + dy;
    posB[2] = bz + dz;
    if (GetStereoMode() == StereoMode::ExecViewConstDual) {
      g_vsPatchCam[0] = cx;
      g_vsPatchCam[1] = cy;
      g_vsPatchCam[2] = cz;
      g_vsPatchDelta[0] = dx;
      g_vsPatchDelta[1] = dy;
      g_vsPatchDelta[2] = dz;
      g_vsPatchOn.store(true);
    }
    g_execViewStage = 5;  // exec pass 2
    RunExecuteEyePass(edx);
    g_vsPatchOn.store(false);
    g_execViewStage = 6;  // capture R
    if (CopyCurrentRtToEyeCanvas(g_device, g_texR, vr::Eye_Right))
      g_haveR = true;
    g_execViewStage = 7;  // restore
    posA[0] = ax;
    posA[1] = ay;
    posA[2] = az;
    posB[0] = bx;
    posB[1] = by;
    posB[2] = bz;
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    return false;
  }
}

void RunExecViewPhaseDual(void* edx) {
  g_inDual.store(true);
  const bool ok = RunExecViewPhaseDualGuarded(edx);
  g_inDual.store(false);
  g_vsPatchOn.store(false);
  g_dualDoneThisFrame = true;
  g_skipExecC = 1;
  g_skipExecD = 1;
  if (!ok) {
    g_execDualDead.store(true);
    g_haveL = g_haveR = false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    Log("StereoExecPhase: EXCEPTION stage=%ld code=0x%08X addr=%p (exeRva 0x%X) - "
        "PERMANENTLY disabled, native mono continues",
        g_execViewStage, g_execViewExcCode, reinterpret_cast<void*>(g_execViewExcAddr),
        static_cast<unsigned>(g_execViewExcAddr - base));
    return;
  }
  const uint32_t n = ++g_execViewDualCount;
  if (n <= 6 || (n % 300) == 0)
    Log("StereoExecPhase: #%u haveL=%d haveR=%d skips=%u sep=%.0fcm vsPatch=%u", n,
        g_haveL ? 1 : 0, g_haveR ? 1 : 0, g_execViewSkips.load(),
        GetStereoSepMeters() * 100.f, StereoVsTranslateCount());
}

void __fastcall HookExecA(void* self, void* edx) {
  if (!g_origExecA)
    return;
  g_lastThisPhaseA = self;
  const StereoMode mode = GetStereoMode();
  if (mode == StereoMode::RenderRootProbe)
    LogRenderRootRet("ExecPhaseA", _ReturnAddress());
  if (NeedsExecHooks(mode) && g_skipExecA > 0 && !g_inDual.load()) {
    --g_skipExecA;
    return;
  }
  if (g_inDual.load()) {
    g_origExecA(self, edx);
    return;
  }
  // Mode 23/24: per-phase double exec + exec-side cam matrix shift (+ VS const).
  if (IsExecViewPhaseMode(mode)) {
    if (g_execDualDead.load() || g_dualDoneThisFrame || !IsCamMatrixOverrideEnabled() ||
        !g_lastThisDraw || !g_origExecD || !g_texL || !g_texR || !g_device) {
      g_origExecA(self, edx);
      return;
    }
    RunExecViewPhaseDual(edx);
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
  if (GetStereoMode() == StereoMode::RenderRootProbe)
    LogRenderRootRet("ExecPhaseC", _ReturnAddress());
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
  if (GetStereoMode() == StereoMode::RenderRootProbe)
    LogRenderRootRet("ExecDrawScene", _ReturnAddress());
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

  // Fusion baseline: nullptr bounds + temporal IPD only.
  // Soft-inset/square UV experiments reduced or killed fusion — keep off until engine FOV works.
  const vr::EVRCompositorError err =
      vr::VRCompositor()->Submit(eye, &t, nullptr, vr::Submit_Default);
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
  if (mode == StereoMode::RenderRootProbe)
    LogRenderRootRet("BuildDrawScene", _ReturnAddress());

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
  if (mode == StereoMode::RenderRootProbe)
    LogRenderRootRet("BuildPhaseA", _ReturnAddress());
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
  if (mode == StereoMode::RenderRootProbe)
    LogRenderRootRet("BuildPhaseC", _ReturnAddress());
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
  const bool canvas = UsesAngleCorrectCanvas(GetStereoMode());
  // Mode 14: angle-correct canvas placement. Others: prefer current color RT; BB fallback.
  if (eye == StereoEye::Left) {
    const bool ok = canvas ? CopyBbToEyeCanvas(device, g_texL, vr::Eye_Left)
                           : CopyCurrentColorToEye(device, g_texL);
    if (ok)
      g_haveL = true;
    SetStereoEye(StereoEye::Right);
  } else {
    const bool ok = canvas ? CopyBbToEyeCanvas(device, g_texR, vr::Eye_Right)
                           : CopyCurrentColorToEye(device, g_texR);
    if (ok)
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
    if (mode == StereoMode::CamFovProbe)
      Log("StereoRender: mode 16 CamFovProbe (mode 14 rendering + READ-ONLY FOV probe; "
          "look for FovProbe lines, then feed offset to mode 17)");
    else if (mode == StereoMode::CamFovWrite)
      Log("StereoRender: mode 17 CamFovWrite (mode 14 rendering + engine FOV write from "
          "gtaiv_dxvk_vr.fovpatch)");
    else if (mode == StereoMode::CanvasWide)
      Log("StereoRender: mode 15 CanvasWide — TESTED DEAD (Rage ignores D3DTS_PROJECTION), "
          "use mode 14/16/17");
    else if (mode == StereoMode::GeometryCanvas)
      Log("StereoRender: mode 14 GeometryCanvas (temporal + per-eye angle-correct canvas; "
          "black border is EXPECTED)");
    else if (mode == StereoMode::StereoFusion)
      Log("StereoRender: mode 13 StereoFusion (L4D2/BotW: temporal + coverFOV + TextureBounds)");
    else
      Log("StereoRender: mode 4 temporal IPD");
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 0 if freeze");
    return true;
  }

  if (mode == StereoMode::RootDispatchProbe) {
    g_ok = InstallRootProbeHooks(kNumRoots);
    Log("StereoRender: mode 19 ROOT-DISPATCH probe (passthrough counters on %d candidate "
        "roots; look for 'RootProbe:' lines) ok=%d",
        kNumRoots, g_ok.load() ? 1 : 0);
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 0 if freeze");
    return g_ok.load();
  }

  if (mode == StereoMode::ViewMatrixScan) {
    g_ok = InstallRootProbeHooks(kNumRoots);
    Log("StereoRender: mode 21 VIEW-MATRIX scan (READ-ONLY; look for 'ViewScan:' lines; "
        "stand still in gameplay ~30s) ok=%d",
        g_ok.load() ? 1 : 0);
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 0 if freeze");
    return g_ok.load();
  }

  if (mode == StereoMode::ExecViewDual) {
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_execDualDead.store(false);
    g_execViewDualCount.store(0);
    g_execViewSkips.store(0);
    g_ok = InstallRootProbeHooks(1);  // only ExecRoot needed
    Log("StereoRender: mode 22 EXEC-VIEW dual (ExecRoot x2 on render thread, cam matrices "
        "@0x%X/0x%X shifted between passes; expect ~half FPS, no L/R jumping) ok=%d",
        kMgrCamPosRvaA, kMgrCamPosRvaB, g_ok.load() ? 1 : 0);
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 0 if freeze");
    return g_ok.load();
  }

  if (mode == StereoMode::SameFrameRootDual) {
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_loggedIpd = false;
    g_rootDualCount.store(0);
    g_ok = InstallRootProbeHooks(kNumRoots);
    Log("StereoRender: mode 20 SAME-FRAME ROOT dual (BuildRootA x2 per frame, both eyes "
        "in ONE game tick; expect ~half FPS but no L/R jumping) ok=%d",
        g_ok.load() ? 1 : 0);
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 0 if freeze");
    return g_ok.load();
  }

  if (IsExecViewPhaseMode(mode)) {
    SetStereoEye(StereoEye::Left);
    g_phaseLogLeft.store(0);
    g_dualDoneThisFrame = false;
    g_skipExecA = g_skipExecC = g_skipExecD = 0;
    g_skipBuildC = g_skipBuildD = 0;
    g_haveL = g_haveR = false;
    g_execAHooked = g_execCHooked = g_execDHooked = false;
    g_origExecA = g_origExecC = g_origExecD = nullptr;
    g_execDualDead.store(false);
    g_execViewDualCount.store(0);
    g_execViewSkips.store(0);
    g_vsPatchOn.store(false);
    g_ok = InstallAllThreePhases();
    if (mode == StereoMode::ExecViewConstDual)
      Log("StereoRender: mode 24 EXEC-VIEW CONST dual (mode 23 + SetVSConstF view-translate "
          "in the R pass; look for 'VsPatch:' lines) ok=%d",
          g_ok.load() ? 1 : 0);
    else
      Log("StereoRender: mode 23 EXEC-VIEW PHASE dual (per-phase Execute x2 like mode 10 + "
          "cam matrices @0x%X/0x%X shifted between passes) ok=%d",
          kMgrCamPosRvaA, kMgrCamPosRvaB, g_ok.load() ? 1 : 0);
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 0 if freeze");
    return g_ok.load();
  }

  if (mode == StereoMode::PhaseProbe || mode == StereoMode::VtableProbe ||
      mode == StereoMode::ExecuteDual || mode == StereoMode::BuildExecDual ||
      mode == StereoMode::D3dCamDual || mode == StereoMode::RenderRootProbe) {
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
    else if (mode == StereoMode::RenderRootProbe)
      Log("StereoRender: mode 18 RENDER-ROOT probe (read-only, mono submit; "
          "look for 'RenderRoot: NEW' lines) ok=%d",
          g_ok.load() ? 1 : 0);
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

  if (mode == StereoMode::SameFrameRootDual || mode == StereoMode::ExecViewDual ||
      IsExecViewPhaseMode(mode)) {
    if (IsExecViewPhaseMode(mode)) {
      g_dualDoneThisFrame = false;
      g_skipExecA = g_skipExecC = g_skipExecD = 0;
    }
    if (!IsCamMatrixOverrideEnabled())
      return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
      return;
    D3DSURFACE_DESC desc{};
    bb->GetDesc(&desc);
    bb->Release();
    uint32_t rtW = desc.Width, rtH = desc.Height;
    ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
    EnsureEyeRts(device, rtW, rtH);
    return;  // captures happen inside the root/phase hooks
  }

  if (mode == StereoMode::RootDispatchProbe) {
    static uint32_t s_frame = 0;
    ++s_frame;
    uint32_t len = g_rootOrderLen.exchange(0);
    if (len > sizeof(g_rootOrder) - 1)
      len = sizeof(g_rootOrder) - 1;
    g_rootOrder[len] = 0;
    const uint32_t c0 = g_rootCount[0].exchange(0);
    const uint32_t c1 = g_rootCount[1].exchange(0);
    const uint32_t c2 = g_rootCount[2].exchange(0);
    const uint32_t c3 = g_rootCount[3].exchange(0);
    const uint32_t c4 = g_rootCount[4].exchange(0);
    if (s_frame <= 12 || (s_frame % 300) == 0)
      Log("RootProbe: frame=%u %s=%u %s=%u %s=%u %s=%u %s=%u order=%s", s_frame, kRootName[0],
          c0, kRootName[1], c1, kRootName[2], c2, kRootName[3], c3, kRootName[4], c4,
          g_rootOrder);
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
  uint32_t rtW = desc.Width, rtH = desc.Height;
  if (UsesAngleCorrectCanvas(mode))
    ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
  if (!EnsureEyeRts(device, rtW, rtH))
    return;
  TemporalCaptureThisFrame(device);
}

bool StereoTrySubmitEyes(IDirect3DDevice9* device, ID3D9VkInteropDevice* interop) {
  const StereoMode mode = GetStereoMode();
  if (!IsTemporalStereoMode(mode) && mode != StereoMode::SameFrameDual &&
      mode != StereoMode::GBufferRtDual && mode != StereoMode::FusionSwap &&
      mode != StereoMode::ExecuteDual && mode != StereoMode::BuildExecDual &&
      mode != StereoMode::D3dCamDual && mode != StereoMode::SameFrameRootDual &&
      mode != StereoMode::ExecViewDual && !IsExecViewPhaseMode(mode))
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

bool StereoVsGetPatchParams(float cam3[3], float delta3[3]) {
  if (!g_vsPatchOn.load(std::memory_order_relaxed))
    return false;
  cam3[0] = g_vsPatchCam[0];
  cam3[1] = g_vsPatchCam[1];
  cam3[2] = g_vsPatchCam[2];
  delta3[0] = g_vsPatchDelta[0];
  delta3[1] = g_vsPatchDelta[1];
  delta3[2] = g_vsPatchDelta[2];
  return true;
}

}  // namespace asi
