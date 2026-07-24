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
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

// Mode 30 phase machine (no prologue hooks — reuses Mode 23 exec path).
enum class Mode30Phase : int { SoftStart = 0, Dual = 1, PairHold = 2 };
std::atomic<int> g_mode30Phase{static_cast<int>(Mode30Phase::SoftStart)};
std::atomic<uint32_t> g_mode30SoftEs{0};
std::atomic<uint32_t> g_mode30DevPatches{0};
std::atomic<uint32_t> g_mode30ZeroDiff{0};
// Pair-hold: capture into hold RTs; promote to submit RTs only when L+R pair is complete.
IDirect3DTexture9* g_holdL = nullptr;
IDirect3DTexture9* g_holdR = nullptr;
bool g_pairAwaitingR = false;
std::atomic<uint32_t> g_pairPromoteCount{0};

// ---- Mode 31: discover ~1×/frame VsRet walker → same-frame dual + VS patch ----
enum class Mode31Phase : int {
  SoftPairHold = 0,  // Mode 30 path while cam warms / discover arms
  Discover = 1,      // SetVSConstF stack histogram (no game prologue hooks)
  Count = 2,         // count-only thiscall hook ≥45 EndScenes
  Dual = 3,          // walk×2 + VS translate between passes
  FallbackPairHold = 4
};
std::atomic<int> g_mode31Phase{static_cast<int>(Mode31Phase::SoftPairHold)};
std::atomic<uint32_t> g_mode31Es{0};
std::atomic<uint32_t> g_mode31CountEs{0};
std::atomic<uint32_t> g_mode31DualN{0};
std::atomic<uint32_t> g_mode31ZeroDiff{0};
std::atomic<uint32_t> g_mode31VsCallsFrame{0};
uint32_t g_mode31VsTid = 0;
uint32_t g_mode31EntrySum = 0;

struct Mode31FrameHit {
  uint32_t rva;
  uint32_t count;
};
Mode31FrameHit g_mode31Frame[96]{};
int g_mode31FrameN = 0;

struct Mode31Agg {
  uint32_t rva;
  uint32_t rareFrames;  // EndScenes where this RVA appeared 1..4 times
  uint32_t totalHits;
};
Mode31Agg g_mode31Agg[64]{};
int g_mode31AggN = 0;

uintptr_t g_mode31TryList[8]{};
int g_mode31TryN = 0;
int g_mode31TryIdx = 0;
uint32_t g_mode31ActiveRva = 0;

// ---- Mode 32: HookSetVSConstF-depth VsParent discover → count → dual ----
enum class Mode32Phase : int {
  SoftPairHold = 0,
  Discover = 1,
  Count = 2,
  Dual = 3,
  FallbackPairHold = 4
};
std::atomic<int> g_mode32Phase{static_cast<int>(Mode32Phase::SoftPairHold)};
std::atomic<uint32_t> g_mode32Es{0};
std::atomic<uint32_t> g_mode32CountEs{0};
std::atomic<uint32_t> g_mode32DualN{0};
std::atomic<uint32_t> g_mode32ZeroDiff{0};
std::atomic<uint32_t> g_mode32VsCallsFrame{0};
uint32_t g_mode32VsTid = 0;
uint32_t g_mode32EntrySum = 0;

struct Mode32FrameHit {
  uint32_t rva;
  uint32_t count;
};
Mode32FrameHit g_mode32Frame[96]{};
int g_mode32FrameN = 0;

struct Mode32Agg {
  uint32_t rva;
  uint32_t rareFrames;
  uint32_t totalHits;
};
Mode32Agg g_mode32Agg[64]{};
int g_mode32AggN = 0;

uintptr_t g_mode32TryList[8]{};
int g_mode32TryN = 0;
int g_mode32TryIdx = 0;
uint32_t g_mode32ActiveRva = 0;

// ---- Mode 33: wait for live VsParent samples → CC-pad thiscall0 → dual ----
enum class Mode33Phase : int {
  SoftWaitSamples = 0,  // pair-hold + collect; do NOT seed while hist empty
  Discover = 1,
  Count = 2,
  Dual = 3,
  FallbackPairHold = 4
};
std::atomic<int> g_mode33Phase{static_cast<int>(Mode33Phase::SoftWaitSamples)};
std::atomic<uint32_t> g_mode33Es{0};
std::atomic<uint32_t> g_mode33CountEs{0};
std::atomic<uint32_t> g_mode33DualN{0};
std::atomic<uint32_t> g_mode33ZeroDiff{0};
std::atomic<uint32_t> g_mode33VsCallsFrame{0};
uint32_t g_mode33VsTid = 0;
uint32_t g_mode33EntrySum = 0;
uint32_t g_mode33SampleHits = 0;  // live VsParent notes seen

struct Mode33FrameHit {
  uint32_t rva;
  uint32_t count;
};
Mode33FrameHit g_mode33Frame[96]{};
int g_mode33FrameN = 0;

struct Mode33Agg {
  uint32_t rva;
  uint32_t rareFrames;
  uint32_t totalHits;
};
Mode33Agg g_mode33Agg[64]{};
int g_mode33AggN = 0;

uintptr_t g_mode33TryList[8]{};
int g_mode33TryN = 0;
int g_mode33TryIdx = 0;
uint32_t g_mode33ActiveRva = 0;

// ---- Mode 34: VsRet(0x2C73E) caller → resolve FUNCTION STARTS → dual ----
enum class Mode34Phase : int {
  SoftWaitSamples = 0,
  Discover = 1,
  Count = 2,
  Dual = 3,
  FallbackPairHold = 4
};
std::atomic<int> g_mode34Phase{static_cast<int>(Mode34Phase::SoftWaitSamples)};
std::atomic<uint32_t> g_mode34Es{0};
std::atomic<uint32_t> g_mode34CountEs{0};
std::atomic<uint32_t> g_mode34DualN{0};
std::atomic<uint32_t> g_mode34ZeroDiff{0};
std::atomic<uint32_t> g_mode34VsCallsFrame{0};
uint32_t g_mode34VsTid = 0;
uint32_t g_mode34EntrySum = 0;
uint32_t g_mode34SampleHits = 0;
uint32_t g_mode34VsRetHits = 0;  // HookSetVSConstF with ret==0x2C73E

struct Mode34FrameHit {
  uint32_t rva;
  uint32_t count;
};
Mode34FrameHit g_mode34Frame[96]{};
int g_mode34FrameN = 0;

struct Mode34Agg {
  uint32_t rva;
  uint32_t rareFrames;
  uint32_t totalHits;
};
Mode34Agg g_mode34Agg[64]{};
int g_mode34AggN = 0;

uintptr_t g_mode34TryList[8]{};
int g_mode34TryN = 0;
int g_mode34TryIdx = 0;
uint32_t g_mode34ActiveRva = 0;

enum class Mode32Abi : int {
  Reject = 0,
  Thiscall = 1,   // safe HookDrawWalk dual
  Stdcall = 2,    // log only — do not hook (wrong arity risk)
  Fastcall = 3    // ecx+edx — count-only only if clearly thiscall-shaped
};

// MSAA resolve target for canvas captures (sub-rect StretchRect from an MSAA
// source is illegal in D3D9 — resolve full-surface first, then sub-rect copy).
IDirect3DSurface9* g_resolveSurf = nullptr;
uint32_t g_resolveW = 0, g_resolveH = 0;
D3DFORMAT g_resolveFmt = D3DFMT_UNKNOWN;

// L-vs-R image diff probe (mode 23/24): tiny downsample + readback.
IDirect3DSurface9* g_diffSmall[2] = {};
IDirect3DSurface9* g_diffSys[2] = {};

void ReleaseEyeRts() {
  if (g_texL) {
    g_texL->Release();
    g_texL = nullptr;
  }
  if (g_texR) {
    g_texR->Release();
    g_texR = nullptr;
  }
  if (g_holdL) {
    g_holdL->Release();
    g_holdL = nullptr;
  }
  if (g_holdR) {
    g_holdR->Release();
    g_holdR = nullptr;
  }
  if (g_resolveSurf) {
    g_resolveSurf->Release();
    g_resolveSurf = nullptr;
    g_resolveW = g_resolveH = 0;
    g_resolveFmt = D3DFMT_UNKNOWN;
  }
  for (int i = 0; i < 2; ++i) {
    if (g_diffSmall[i]) {
      g_diffSmall[i]->Release();
      g_diffSmall[i] = nullptr;
    }
    if (g_diffSys[i]) {
      g_diffSys[i]->Release();
      g_diffSys[i] = nullptr;
    }
  }
  g_rtW = g_rtH = 0;
  g_haveL = g_haveR = false;
  g_pairAwaitingR = false;
}

bool EnsureEyeRts(IDirect3DDevice9* dev, uint32_t w, uint32_t h) {
  if (!dev || w < 16 || h < 16)
    return false;
  if (g_texL && g_texR && g_holdL && g_holdR && g_device == dev && g_rtW == w && g_rtH == h)
    return true;
  ReleaseEyeRts();
  g_device = dev;
  if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                &g_texL, nullptr)) ||
      FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                &g_texR, nullptr)) ||
      FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                &g_holdL, nullptr)) ||
      FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                &g_holdR, nullptr))) {
    ReleaseEyeRts();
    Log("StereoRender: CreateTexture RT FAIL %ux%u", w, h);
    return false;
  }
  g_rtW = w;
  g_rtH = h;
  Log("StereoRender: eye RTs %ux%u OK (submit+hold)", w, h);
  return true;
}

bool PromoteHoldPair(IDirect3DDevice9* dev) {
  if (!dev || !g_texL || !g_texR || !g_holdL || !g_holdR)
    return false;
  IDirect3DSurface9 *srcL = nullptr, *srcR = nullptr, *dstL = nullptr, *dstR = nullptr;
  if (FAILED(g_holdL->GetSurfaceLevel(0, &srcL)) || !srcL ||
      FAILED(g_holdR->GetSurfaceLevel(0, &srcR)) || !srcR ||
      FAILED(g_texL->GetSurfaceLevel(0, &dstL)) || !dstL ||
      FAILED(g_texR->GetSurfaceLevel(0, &dstR)) || !dstR) {
    if (srcL) srcL->Release();
    if (srcR) srcR->Release();
    if (dstL) dstL->Release();
    if (dstR) dstR->Release();
    return false;
  }
  const bool ok = SUCCEEDED(dev->StretchRect(srcL, nullptr, dstL, nullptr, D3DTEXF_NONE)) &&
                  SUCCEEDED(dev->StretchRect(srcR, nullptr, dstR, nullptr, D3DTEXF_NONE));
  srcL->Release();
  srcR->Release();
  dstL->Release();
  dstR->Release();
  if (ok) {
    g_haveL = g_haveR = true;
    const uint32_t n = ++g_pairPromoteCount;
    if (n <= 4 || (n % 300) == 0)
      Log("StereoPairHold: promoted pair #%u (both eyes update together)", n);
  }
  return ok;
}

// Mode 30: translate view/viewProj blocks already resident in device VS regs
// (re-exec issues zero SetVSConstF — Mode 24). Same match math as HookSetVSConstF.
int PushDeviceVsEyeTranslate(IDirect3DDevice9* dev, const float cam[3], const float d[3]) {
  if (!dev || !cam || !d)
    return 0;
  const float cx = cam[0], cy = cam[1], cz = cam[2];
  const float dx = d[0], dy = d[1], dz = d[2];
  int patched = 0;
  for (UINT base = 0; base <= 60; base += 4) {
    float f[16]{};
    if (FAILED(dev->GetVertexShaderConstantF(base, f, 4)))
      continue;
    const float xa = cx * f[0] + cy * f[4] + cz * f[8] + f[12];
    const float ya = cx * f[1] + cy * f[5] + cz * f[9] + f[13];
    const float za = cx * f[2] + cy * f[6] + cz * f[10] + f[14];
    const float wa = cx * f[3] + cy * f[7] + cz * f[11] + f[15];
    const float gxa = std::fabs(cx * f[0]) + std::fabs(cy * f[4]) + std::fabs(cz * f[8]) +
                      std::fabs(f[12]);
    const float gya = std::fabs(cx * f[1]) + std::fabs(cy * f[5]) + std::fabs(cz * f[9]) +
                      std::fabs(f[13]);
    const float xb = cx * f[0] + cy * f[1] + cz * f[2] + f[3];
    const float yb = cx * f[4] + cy * f[5] + cz * f[6] + f[7];
    const float zb = cx * f[8] + cy * f[9] + cz * f[10] + f[11];
    const float wb = cx * f[12] + cy * f[13] + cz * f[14] + f[15];
    const float gxb = std::fabs(cx * f[0]) + std::fabs(cy * f[1]) + std::fabs(cz * f[2]) +
                      std::fabs(f[3]);
    const float gyb = std::fabs(cx * f[4]) + std::fabs(cy * f[5]) + std::fabs(cz * f[6]) +
                      std::fabs(f[7]);
    const bool matchA = std::fabs(xa) < 2e-3f * gxa + 1e-3f &&
                        std::fabs(ya) < 2e-3f * gya + 1e-3f &&
                        (std::fabs(za) > 1e-3f || std::fabs(wa) > 0.5f) && gxa > 1e-3f;
    const bool matchB = std::fabs(xb) < 2e-3f * gxb + 1e-3f &&
                        std::fabs(yb) < 2e-3f * gyb + 1e-3f &&
                        (std::fabs(zb) > 1e-3f || std::fabs(wb) > 0.5f) && gxb > 1e-3f;
    if (matchA) {
      f[12] -= dx * f[0] + dy * f[4] + dz * f[8];
      f[13] -= dx * f[1] + dy * f[5] + dz * f[9];
      f[14] -= dx * f[2] + dy * f[6] + dz * f[10];
      f[15] -= dx * f[3] + dy * f[7] + dz * f[11];
    } else if (matchB) {
      f[3] -= dx * f[0] + dy * f[1] + dz * f[2];
      f[7] -= dx * f[4] + dy * f[5] + dz * f[6];
      f[11] -= dx * f[8] + dy * f[9] + dz * f[10];
      f[15] -= dx * f[12] + dy * f[13] + dz * f[14];
    } else {
      continue;
    }
    if (SUCCEEDED(dev->SetVertexShaderConstantF(base, f, 4)))
      ++patched;
  }
  return patched;
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

  static uint32_t s_loggedGen = 0xFFFFFFFFu;
  const uint32_t gen = GetGameFovPublishGeneration();
  if (gen != s_loggedGen && gen > 0) {
    s_loggedGen = gen;
    const float fillH = (coverH > 0.05f) ? (gameH / coverH) : 0.f;
    const float fillV = (coverV > 0.05f) ? (gameV / coverV) : 0.f;
    Log("StereoCanvasSize: bb=%ux%u canvas=%ux%u sx=%.2f sy=%.2f gameTan=(%.3f,%.3f) "
        "coverTan=(%.3f,%.3f) fill≈%.0f%%h/%.0f%%v trueFov=%d gen=%u",
        bbW, bbH, w, h, sx, sy, gameH, gameV, coverH, coverV, fillH * 100.f, fillV * 100.f,
        IsGameFovFromCCamActive() ? 1 : 0, gen);
  }
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

  static bool s_srcLogged = false;
  if (!s_srcLogged) {
    s_srcLogged = true;
    Log("StereoCapture: src %ux%u fmt=%u msaa=%d", sd.Width, sd.Height,
        static_cast<unsigned>(sd.Format), static_cast<int>(sd.MultiSampleType));
  }

  // MSAA source: full-surface resolve into a cached plain RT, then sub-rect
  // from the resolve (sub-rect StretchRect straight from MSAA = INVALIDCALL,
  // which forced the geometry-breaking full-stretch fallback before).
  if (sd.MultiSampleType != D3DMULTISAMPLE_NONE) {
    if (g_resolveSurf &&
        (g_resolveW != sd.Width || g_resolveH != sd.Height || g_resolveFmt != sd.Format)) {
      g_resolveSurf->Release();
      g_resolveSurf = nullptr;
    }
    if (!g_resolveSurf) {
      if (SUCCEEDED(dev->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE,
                                            0, FALSE, &g_resolveSurf, nullptr)) &&
          g_resolveSurf) {
        g_resolveW = sd.Width;
        g_resolveH = sd.Height;
        g_resolveFmt = sd.Format;
        Log("StereoCapture: MSAA resolve RT %ux%u created", sd.Width, sd.Height);
      } else {
        g_resolveSurf = nullptr;
      }
    }
    if (g_resolveSurf && SUCCEEDED(dev->StretchRect(bb, nullptr, g_resolveSurf, nullptr,
                                                    D3DTEXF_NONE))) {
      bb->Release();
      bb = g_resolveSurf;
      bb->AddRef();
    }
  }
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
  static uint32_t s_loggedGen = 0;
  const uint32_t gen = GetGameFovPublishGeneration();
  if (gen != s_loggedGen) {
    s_loggedL = s_loggedR = false;
    s_loggedGen = gen;
  }
  bool* logged = (eye == vr::Eye_Right) ? &s_loggedR : &s_loggedL;
  if (!*logged) {
    *logged = true;
    Log("StereoCanvas: %s canvas=%ux%u dst=(%ld,%ld)-(%ld,%ld) src=(%ld,%ld)-(%ld,%ld) "
        "gameTan=(%.3f,%.3f) eyeTan=(%.3f..%.3f, %.3f..%.3f) trueFov=%d gen=%u",
        eye == vr::Eye_Right ? "R" : "L", dd.Width, dd.Height, rc.left, rc.top, rc.right,
        rc.bottom, src.left, src.top, src.right, src.bottom, gameH, gameV, l, r, t, b,
        IsGameFovFromCCamActive() ? 1 : 0, gen);
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
// Mode 24/25 patch params for the SetVertexShaderConstantF view-translate hook
// (stereo_proj.cpp). Written and read on the same thread as the dual pass.
std::atomic<bool> g_vsPatchOn{false};
float g_vsPatchCam[3] = {};
float g_vsPatchDelta[3] = {};
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

// Mode 25: the REAL draws happen inline during the build walk (mode 24 evidence:
// all VS-constant uploads flow through one wrapper site on one thread, and NONE
// during the phase re-exec). Mode 20 v1 already ran the walk twice per frame
// (true double render, 50 FPS) — it only lacked a camera change that reaches the
// render side. Combine: walk 1 = left, then shift the manager cam matrices AND
// arm the VS-constant translate, walk 2 = right. No double shift possible: the
// VS detection keys on the LEFT cam pos, which no longer matches if the manager
// shift already reached the constants.
long long CompareEyeCanvases(IDirect3DDevice9* dev);

bool RunBuildDualViewShiftGuarded(void* self, void* edx) {
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
      g_origRoot[1](self, edx);
      return true;
    }
    g_execViewStage = 2;  // walk 1 = LEFT (CCam snapshot is the left eye)
    g_origRoot[1](self, edx);
    g_execViewStage = 3;  // capture L
    if (CopyBbToEyeCanvas(g_device, g_texL, vr::Eye_Left))
      g_haveL = true;
    g_execViewStage = 4;  // shift manager matrices + arm VS translate
    const float ax = posA[0], ay = posA[1], az = posA[2];
    const float bx = posB[0], by = posB[1], bz = posB[2];
    posA[0] = ax + dx;
    posA[1] = ay + dy;
    posA[2] = az + dz;
    posB[0] = bx + dx;
    posB[1] = by + dy;
    posB[2] = bz + dz;
    g_vsPatchCam[0] = cx;
    g_vsPatchCam[1] = cy;
    g_vsPatchCam[2] = cz;
    g_vsPatchDelta[0] = dx;
    g_vsPatchDelta[1] = dy;
    g_vsPatchDelta[2] = dz;
    g_vsPatchOn.store(true);
    g_execViewStage = 5;  // walk 2 = RIGHT
    g_origRoot[1](self, edx);
    g_vsPatchOn.store(false);
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
    g_vsPatchOn.store(false);
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

  // Mode 26: do NOT double-call BuildRootA (freeze risk; also produced only ONE
  // EndScene per tick so it never gave same-frame stereo). Single native build;
  // EndScene on the replay thread does temporal L/R via VS-const arm flip.
  if (mode == StereoMode::RecordDualReplayShift || mode == StereoMode::SameFrameWrapDual ||
      mode == StereoMode::ReplayRootProbe || mode == StereoMode::VsParentCountProbe) {
    g_origRoot[1](self, edx);
    const uint32_t n = ++g_execViewDualCount;
    if (n <= 3 || (n % 600) == 0)
      Log("StereoRecDual: build #%u (game thread %u) - temporal VS arm on EndScene", n,
          GetCurrentThreadId());
    return;
  }

  if (mode == StereoMode::BuildDualViewShift && !g_execDualDead.load() && !g_inDual.load() &&
      IsCamMatrixOverrideEnabled() && g_device && g_texL && g_texR) {
    static bool s_tidLogged = false;
    if (!s_tidLogged) {
      s_tidLogged = true;
      Log("StereoBuildShift: build threadId=%u (compare with VsRet tid)", GetCurrentThreadId());
    }
    g_inDual.store(true);
    const bool ok = RunBuildDualViewShiftGuarded(self, edx);
    g_inDual.store(false);
    if (!ok) {
      g_execDualDead.store(true);
      g_haveL = g_haveR = false;
      const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
      Log("StereoBuildShift: EXCEPTION stage=%ld code=0x%08X addr=%p (exeRva 0x%X) - "
          "PERMANENTLY disabled, native mono continues",
          g_execViewStage, g_execViewExcCode, reinterpret_cast<void*>(g_execViewExcAddr),
          static_cast<unsigned>(g_execViewExcAddr - base));
      g_origRoot[1](self, edx);
      return;
    }
    const uint32_t n = ++g_execViewDualCount;
    if (n <= 6 || (n % 300) == 0)
      Log("StereoBuildShift: #%u haveL=%d haveR=%d skips=%u sep=%.0fcm vsPatch=%u "
          "vsCallsTotal=%u vsCallsR=%u",
          n, g_haveL ? 1 : 0, g_haveR ? 1 : 0, g_execViewSkips.load(),
          GetStereoSepMeters() * 100.f, StereoVsTranslateCount(), StereoVsTotalCalls(),
          StereoVsRightPassCalls());
    if (g_haveL && g_haveR && (n == 5 || (n % 300) == 0))
      CompareEyeCanvases(g_device);
    if (n == 600 || n == 3000)
      StereoVsDumpRets();
    return;
  }

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
// Mode 27/29: VsParent candidates. Mode 29 uses live Mode 26 mid-fn return RVAs;
// FindFnStartNear (VirtualQuery-bounded) resolves each to a prologue, then we
// count calls per EndScene → pick the ~1×/frame replay root for Mode 30 dual.
constexpr int kNumReplayCands = 5;
constexpr uint32_t kReplayCandRva[kNumReplayCands] = {
    0x22187,  // Mode 26 VsParent mid-fn ret
    0x37C01,  // Mode 26 VsParent mid-fn ret
    0x37520,  // Mode 26 VsParent mid-fn ret
    0x32BD7,  // Mode 26 VsParent mid-fn ret
    0x4D901B, // Mode 26 VsParent mid-fn ret
};
const char* kReplayCandName[kNumReplayCands] = {
    "Cand22187", "Cand37C01", "Cand37520", "Cand32BD7", "Cand4D901B"};
using ReplayCand_t = void(__fastcall*)(void* self, void* edx);
ReplayCand_t g_origReplayCand[kNumReplayCands] = {};
std::atomic<uint32_t> g_replayCandCount[kNumReplayCands] = {};
std::atomic<bool> g_replayCandDead[kNumReplayCands] = {};

// Forward: VirtualQuery-bounded prologue scan (defined with Mode 28 helpers).
uintptr_t FindFnStartNear(uintptr_t addrInFn);

template <int I>
void __fastcall HookReplayCand(void* self, void* edx) {
  if (g_replayCandDead[I].load()) {
    if (g_origReplayCand[I])
      g_origReplayCand[I](self, edx);
    return;
  }
  ++g_replayCandCount[I];
  __try {
    g_origReplayCand[I](self, edx);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_replayCandDead[I].store(true);
    Log("ReplayProbe: %s EXCEPTION — permanently disabled", kReplayCandName[I]);
  }
}

bool InstallReplayCandHooks() {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  bool any = false;
  void* detours[kNumReplayCands] = {
      reinterpret_cast<void*>(&HookReplayCand<0>),
      reinterpret_cast<void*>(&HookReplayCand<1>),
      reinterpret_cast<void*>(&HookReplayCand<2>),
      reinterpret_cast<void*>(&HookReplayCand<3>),
      reinterpret_cast<void*>(&HookReplayCand<4>),
  };
  for (int i = 0; i < kNumReplayCands; ++i) {
    g_replayCandCount[i].store(0);
    g_replayCandDead[i].store(false);
    g_origReplayCand[i] = nullptr;
    const uintptr_t retAddr = base + kReplayCandRva[i];
    const uintptr_t fnStart = FindFnStartNear(retAddr);
    if (!fnStart) {
      Log("ReplayProbe: %s SKIP no prologue near retRva=0x%X", kReplayCandName[i],
          kReplayCandRva[i]);
      continue;
    }
    const auto* b = reinterpret_cast<const uint8_t*>(fnStart);
    const uint32_t fnRva = static_cast<uint32_t>(fnStart - base);
    Log("ReplayProbe: %s retRva=0x%X -> fnStartRva=0x%X bytes=%02X %02X %02X %02X @ %p",
        kReplayCandName[i], kReplayCandRva[i], fnRva, b[0], b[1], b[2], b[3],
        reinterpret_cast<void*>(fnStart));
    if (MH_CreateHook(reinterpret_cast<void*>(fnStart), detours[i],
                      reinterpret_cast<void**>(&g_origReplayCand[i])) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(fnStart)) != MH_OK) {
      Log("ReplayProbe: %s MH FAIL @ %p (fnStartRva 0x%X)", kReplayCandName[i],
          reinterpret_cast<void*>(fnStart), fnRva);
      g_origReplayCand[i] = nullptr;
      continue;
    }
    Log("ReplayProbe: hooked %s @ %p (fnStartRva 0x%X)", kReplayCandName[i],
        reinterpret_cast<void*>(fnStart), fnRva);
    any = true;
  }
  return any;
}

void DumpReplayCandCounts() {
  static uint32_t s_n = 0;
  static uint32_t s_prev[kNumReplayCands] = {};
  uint32_t delta[kNumReplayCands] = {};
  for (int i = 0; i < kNumReplayCands; ++i) {
    const uint32_t c = g_replayCandCount[i].load();
    delta[i] = c - s_prev[i];
    s_prev[i] = c;
  }
  ++s_n;
  if (s_n <= 8 || (s_n % 120) == 0)
    Log("VsParentCount: frame#%u %s=%u/f %s=%u/f %s=%u/f %s=%u/f %s=%u/f tid=%u "
        "(want ~1/f)",
        s_n, kReplayCandName[0], delta[0], kReplayCandName[1], delta[1],
        kReplayCandName[2], delta[2], kReplayCandName[3], delta[3],
        kReplayCandName[4], delta[4], GetCurrentThreadId());
}

// ---- Mode 28: same-frame via live caller of VS wrapper 0x2C6AC ----
// Learn stack parents of the VS wrapper → find a ~1×/frame draw walker → dual
// invoke (manager cam shift + VS translate on pass 2) + Mode 14 canvas. Until
// dual is armed (or if it dies), EndScene keeps Mode 26 temporal so the HMD
// still has stereo. FusionFix FOV stays 0 (look-up warp).
constexpr uint32_t kVsWrapRva = 0x2C6AC;
using VsWrap_t = void(__stdcall*)(uint32_t, uint32_t, uint32_t);
VsWrap_t g_origVsWrap = nullptr;
std::atomic<bool> g_wrapHooked{false};
std::atomic<uint32_t> g_wrapCalls{0};

enum class WrapPhase : int { Learn = 0, Count = 1, Dual = 2, TemporalOnly = 3 };
std::atomic<int> g_wrapPhase{static_cast<int>(WrapPhase::Learn)};

struct WrapFnCand {
  uintptr_t start;
  uint32_t hits;
  int depth;
};
WrapFnCand g_wrapFns[32]{};
int g_wrapFnN = 0;

using DrawWalk_t = void(__fastcall*)(void* self, void* edx);
DrawWalk_t g_origDrawWalk = nullptr;
void* g_drawWalkAddr = nullptr;
std::atomic<bool> g_drawWalkDead{false};
std::atomic<bool> g_inDrawWalkDual{false};
std::atomic<uint32_t> g_drawWalkDualCount{0};
std::atomic<uint32_t> g_drawWalkEntries{0};
uintptr_t g_wrapTryList[8]{};
int g_wrapTryN = 0;
int g_wrapTryIdx = 0;

uintptr_t FindFnStartNear(uintptr_t addrInFn) {
  if (!addrInFn)
    return 0;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<const void*>(addrInFn), &mbi, sizeof(mbi)))
    return 0;
  if (mbi.State != MEM_COMMIT)
    return 0;
  const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  const uintptr_t regionEnd = regionBase + mbi.RegionSize;
  if (addrInFn < regionBase || addrInFn >= regionEnd)
    return 0;
  // Cap scan window to 0x1000 but never leave the committed region (no blind
  // -0x1000 into unmapped pages — that contributed to Mode 28 AVs).
  uintptr_t low = regionBase;
  if (addrInFn - low > 0x1000)
    low = addrInFn - 0x1000;
  for (uintptr_t p = addrInFn; p > low; --p) {
    if (p + 3 >= regionEnd)
      continue;
    const auto* b = reinterpret_cast<const uint8_t*>(p);
    if (b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC)
      return p;
    if (b[0] == 0x56 && b[1] == 0x57 && b[2] == 0x8B)
      return p;
    if (b[0] == 0x53 && b[1] == 0x56 && b[2] == 0x57 && b[3] == 0x8B)
      return p;
  }
  // Also test p == low (loop stops before low).
  if (low + 3 < regionEnd && low <= addrInFn) {
    const auto* b = reinterpret_cast<const uint8_t*>(low);
    if (b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC)
      return low;
    if (b[0] == 0x56 && b[1] == 0x57 && b[2] == 0x8B)
      return low;
    if (b[0] == 0x53 && b[1] == 0x56 && b[2] == 0x57 && b[3] == 0x8B)
      return low;
  }
  return 0;
}

// Mode 32: prefer a NEAR thiscall prologue (≤0x300). Blind FindFnStartNear often
// hits stdcall helpers (0x370D0) or thiscall+stackarg frames (0x32A40 ret 4).
uintptr_t FindThiscallNear(uintptr_t addrInFn, uint32_t maxDist) {
  if (!addrInFn || maxDist == 0)
    return 0;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<const void*>(addrInFn), &mbi, sizeof(mbi)))
    return 0;
  if (mbi.State != MEM_COMMIT)
    return 0;
  const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  const uintptr_t regionEnd = regionBase + mbi.RegionSize;
  if (addrInFn < regionBase || addrInFn >= regionEnd)
    return 0;
  auto movEcx = [](uint8_t a, uint8_t b) {
    return a == 0x8B && (b == 0xF1 || b == 0xF9 || b == 0xD9 || b == 0xC1 || b == 0xE9 || b == 0xD1);
  };
  uintptr_t low = regionBase;
  if (addrInFn - low > maxDist)
    low = addrInFn - maxDist;
  for (uintptr_t p = addrInFn; p > low; --p) {
    if (p + 6 >= regionEnd)
      continue;
    const auto* b = reinterpret_cast<const uint8_t*>(p);
    // Classic push + mov r,ecx — only accept INT3-padded starts
    if (b[0] == 0x56 && b[1] == 0x57 && movEcx(b[2], b[3]) && p >= 2 &&
        reinterpret_cast<const uint8_t*>(p)[-1] == 0xCC)
      return p;
    if (b[0] == 0x53 && b[1] == 0x56 && b[2] == 0x57 && movEcx(b[3], b[4]) && p >= 2 &&
        reinterpret_cast<const uint8_t*>(p)[-1] == 0xCC)
      return p;
    if (b[0] == 0x53 && b[1] == 0x55 && b[2] == 0x56 && b[3] == 0x57 && movEcx(b[4], b[5]) &&
        p >= 2 && reinterpret_cast<const uint8_t*>(p)[-1] == 0xCC)
      return p;
    if (b[0] == 0x56 && movEcx(b[1], b[2]) && p >= 2 &&
        reinterpret_cast<const uint8_t*>(p)[-1] == 0xCC)
      return p;
    // 83 EC xx … 8B F1 within 14 — INT3-padded only
    if (b[0] == 0x83 && b[1] == 0xEC && p >= 2 &&
        reinterpret_cast<const uint8_t*>(p)[-1] == 0xCC) {
      for (int i = 2; i < 14; ++i) {
        if (movEcx(b[i], b[i + 1]))
          return p;
      }
    }
  }
  return 0;
}

void NoteWrapFn(uintptr_t start, int depth) {
  if (!start)
    return;
  for (int i = 0; i < g_wrapFnN; ++i) {
    if (g_wrapFns[i].start == start) {
      ++g_wrapFns[i].hits;
      if (depth < g_wrapFns[i].depth)
        g_wrapFns[i].depth = depth;
      return;
    }
  }
  if (g_wrapFnN >= 32)
    return;
  g_wrapFns[g_wrapFnN++] = {start, 1, depth};
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  Log("WrapDual: NEW fn cand exeRva=0x%X depth=%d tid=%u",
      static_cast<unsigned>(start - base), depth, GetCurrentThreadId());
}

void __stdcall HookVsWrap(uint32_t a, uint32_t b, uint32_t c) {
  // Rage: ebx = device/this, stdcall 3 stack args. Must not clobber ebx.
  uintptr_t ebxSave = 0;
  __asm { mov ebxSave, ebx }
  if (g_wrapPhase.load() == static_cast<int>(WrapPhase::Learn)) {
    NoteWrapFn(FindFnStartNear(reinterpret_cast<uintptr_t>(_ReturnAddress())), 0);
    void* stack[8]{};
    const USHORT n = CaptureStackBackTrace(1, 6, stack, nullptr);
    for (USHORT i = 0; i < n && i < 4; ++i)
      NoteWrapFn(FindFnStartNear(reinterpret_cast<uintptr_t>(stack[i])), static_cast<int>(i) + 1);
  }
  ++g_wrapCalls;
  __asm { mov ebx, ebxSave }
  g_origVsWrap(a, b, c);
  __asm { mov ebx, ebxSave }
}

bool InstallVsWrapHook() {
  if (g_wrapHooked.load())
    return true;
  const uintptr_t addr = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) + kVsWrapRva;
  if (MH_CreateHook(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(&HookVsWrap),
                    reinterpret_cast<void**>(&g_origVsWrap)) != MH_OK ||
      MH_EnableHook(reinterpret_cast<void*>(addr)) != MH_OK) {
    Log("WrapDual: VS wrapper hook FAIL @ %p", reinterpret_cast<void*>(addr));
    return false;
  }
  g_wrapHooked.store(true);
  Log("WrapDual: hooked VS wrapper @ %p (rva 0x%X) — learning callers",
      reinterpret_cast<void*>(addr), kVsWrapRva);
  return true;
}

bool RunDrawWalkDualGuarded(void* self, void* edx) {
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

    g_vsPatchOn.store(false);
    SetStereoEye(StereoEye::Left);
    g_execViewStage = 2;
    g_origDrawWalk(self, edx);
    g_execViewStage = 3;
    if (g_device && g_texL && CopyBbToEyeCanvas(g_device, g_texL, vr::Eye_Left))
      g_haveL = true;

    if (!sane) {
      // Still captured Left; skip Right dual this tick (cutscene / wrong cam).
      return true;
    }

    const float ax = posA[0], ay = posA[1], az = posA[2];
    const float bx = posB[0], by = posB[1], bz = posB[2];
    posA[0] = ax + dx;
    posA[1] = ay + dy;
    posA[2] = az + dz;
    posB[0] = bx + dx;
    posB[1] = by + dy;
    posB[2] = bz + dz;
    g_vsPatchCam[0] = cx;
    g_vsPatchCam[1] = cy;
    g_vsPatchCam[2] = cz;
    g_vsPatchDelta[0] = dx;
    g_vsPatchDelta[1] = dy;
    g_vsPatchDelta[2] = dz;
    g_vsPatchOn.store(true);
    g_execViewStage = 5;
    g_origDrawWalk(self, edx);
    g_vsPatchOn.store(false);
    g_execViewStage = 6;
    if (g_device && g_texR && CopyBbToEyeCanvas(g_device, g_texR, vr::Eye_Right))
      g_haveR = true;
    g_execViewStage = 7;
    posA[0] = ax;
    posA[1] = ay;
    posA[2] = az;
    posB[0] = bx;
    posB[1] = by;
    posB[2] = bz;
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    g_vsPatchOn.store(false);
    return false;
  }
}

bool CallDrawWalkOnceGuarded(void* self, void* edx) {
  __try {
    g_origDrawWalk(self, edx);
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    return false;
  }
}

bool CallDrawWalkOnceGuarded(void* self, void* edx);
bool RunMode31DualGuarded(void* self, void* edx);
void Mode31FallbackPairHold(const char* why);
bool RunMode32DualGuarded(void* self, void* edx);
void Mode32FallbackPairHold(const char* why);
void Mode33FallbackPairHold(const char* why);
bool RunMode33DualGuarded(void* self, void* edx);
void Mode34FallbackPairHold(const char* why);
bool RunMode34DualGuarded(void* self, void* edx);

void __fastcall HookDrawWalk(void* self, void* edx) {
  if (!g_origDrawWalk)
    return;
  ++g_drawWalkEntries;

  // Mode 34: VsRet-caller walker (same dual body as Mode 32/33).
  if (GetStereoMode() == StereoMode::SameFrameVsRetCallerDual) {
    const int ph = g_mode34Phase.load();
    if (ph == static_cast<int>(Mode34Phase::Count)) {
      if (!CallDrawWalkOnceGuarded(self, edx)) {
        Log("Mode34: EXCEPTION in count passthrough code=0x%08X @exeRva=0x%X - FALLBACK",
            g_execViewExcCode, g_mode34ActiveRva);
        g_drawWalkEntries.store(0xFFFFFFFFu);
        Mode34FallbackPairHold("SEH in count passthrough");
      }
      return;
    }
    if (ph == static_cast<int>(Mode34Phase::Dual) && !g_drawWalkDead.load() &&
        !g_inDrawWalkDual.load() && IsCamMatrixOverrideEnabled() && g_device && g_holdL &&
        g_holdR) {
      g_inDrawWalkDual.store(true);
      const bool ok = RunMode34DualGuarded(self, edx);
      g_inDrawWalkDual.store(false);
      if (!ok) {
        g_drawWalkDead.store(true);
        g_haveL = g_haveR = false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        Log("Mode34: EXCEPTION dual stage=%ld code=0x%08X @exeRva=0x%X", g_execViewStage,
            g_execViewExcCode, static_cast<unsigned>(g_execViewExcAddr - base));
        Mode34FallbackPairHold("SEH in dual pass");
        CallDrawWalkOnceGuarded(self, edx);
        return;
      }
      const uint32_t n = ++g_mode34DualN;
      if (n <= 6 || (n % 300) == 0)
        Log("Mode34: SAME-FRAME #%u haveL=%d haveR=%d vsPatch=%u vsCallsR=%u sep=%.0fcm "
            "fnRva=0x%X",
            n, g_haveL ? 1 : 0, g_haveR ? 1 : 0, StereoVsTranslateCount(),
            StereoVsRightPassCalls(), GetStereoSepMeters() * 100.f, g_mode34ActiveRva);
      return;
    }
    if (!CallDrawWalkOnceGuarded(self, edx)) {
      Log("Mode34: EXCEPTION idle passthrough - disabling walker hook");
      Mode34FallbackPairHold("SEH in idle walker");
    }
    return;
  }

  // Mode 33: same walker path as Mode 32, stricter phases/fallback.
  if (GetStereoMode() == StereoMode::SameFrameLateVsParentDual) {
    const int ph = g_mode33Phase.load();
    if (ph == static_cast<int>(Mode33Phase::Count)) {
      if (!CallDrawWalkOnceGuarded(self, edx)) {
        Log("Mode33: EXCEPTION in count passthrough code=0x%08X @exeRva=0x%X — FALLBACK",
            g_execViewExcCode, g_mode33ActiveRva);
        g_drawWalkEntries.store(0xFFFFFFFFu);
        Mode33FallbackPairHold("SEH in count passthrough");
      }
      return;
    }
    if (ph == static_cast<int>(Mode33Phase::Dual) && !g_drawWalkDead.load() &&
        !g_inDrawWalkDual.load() && IsCamMatrixOverrideEnabled() && g_device && g_holdL &&
        g_holdR) {
      g_inDrawWalkDual.store(true);
      const bool ok = RunMode33DualGuarded(self, edx);
      g_inDrawWalkDual.store(false);
      if (!ok) {
        g_drawWalkDead.store(true);
        g_haveL = g_haveR = false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        Log("Mode33: EXCEPTION dual stage=%ld code=0x%08X @exeRva=0x%X", g_execViewStage,
            g_execViewExcCode, static_cast<unsigned>(g_execViewExcAddr - base));
        Mode33FallbackPairHold("SEH in dual pass");
        CallDrawWalkOnceGuarded(self, edx);
        return;
      }
      const uint32_t n = ++g_mode33DualN;
      if (n <= 6 || (n % 300) == 0)
        Log("Mode33: SAME-FRAME #%u haveL=%d haveR=%d vsPatch=%u vsCallsR=%u sep=%.0fcm "
            "fnRva=0x%X",
            n, g_haveL ? 1 : 0, g_haveR ? 1 : 0, StereoVsTranslateCount(),
            StereoVsRightPassCalls(), GetStereoSepMeters() * 100.f, g_mode33ActiveRva);
      return;
    }
    if (!CallDrawWalkOnceGuarded(self, edx)) {
      Log("Mode33: EXCEPTION idle passthrough — disabling walker hook");
      Mode33FallbackPairHold("SEH in idle walker");
    }
    return;
  }

  // Mode 32: count-only or same-frame dual on a verified thiscall walker.
  if (GetStereoMode() == StereoMode::SameFrameVsParentDual) {
    const int ph = g_mode32Phase.load();
    if (ph == static_cast<int>(Mode32Phase::Count)) {
      if (!CallDrawWalkOnceGuarded(self, edx)) {
        Log("Mode32: EXCEPTION in count passthrough code=0x%08X @exeRva=0x%X — "
            "immediate FALLBACK (caller may be corrupt; no try-next in-hook)",
            g_execViewExcCode, g_mode32ActiveRva);
        g_drawWalkEntries.store(0xFFFFFFFFu);
        Mode32FallbackPairHold("SEH in count passthrough");
      }
      return;
    }
    if (ph == static_cast<int>(Mode32Phase::Dual) && !g_drawWalkDead.load() &&
        !g_inDrawWalkDual.load() && IsCamMatrixOverrideEnabled() && g_device && g_holdL &&
        g_holdR) {
      g_inDrawWalkDual.store(true);
      const bool ok = RunMode32DualGuarded(self, edx);
      g_inDrawWalkDual.store(false);
      if (!ok) {
        g_drawWalkDead.store(true);
        g_haveL = g_haveR = false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        Log("Mode32: EXCEPTION dual stage=%ld code=0x%08X @exeRva=0x%X", g_execViewStage,
            g_execViewExcCode, static_cast<unsigned>(g_execViewExcAddr - base));
        Mode32FallbackPairHold("SEH in dual pass");
        CallDrawWalkOnceGuarded(self, edx);
        return;
      }
      const uint32_t n = ++g_mode32DualN;
      if (n <= 6 || (n % 300) == 0)
        Log("Mode32: SAME-FRAME #%u haveL=%d haveR=%d vsPatch=%u vsCallsR=%u sep=%.0fcm "
            "fnRva=0x%X",
            n, g_haveL ? 1 : 0, g_haveR ? 1 : 0, StereoVsTranslateCount(),
            StereoVsRightPassCalls(), GetStereoSepMeters() * 100.f, g_mode32ActiveRva);
      return;
    }
    if (!CallDrawWalkOnceGuarded(self, edx)) {
      Log("Mode32: EXCEPTION idle passthrough — disabling walker hook");
      Mode32FallbackPairHold("SEH in idle walker");
    }
    return;
  }

  // Mode 31: count-only or same-frame dual on a verified thiscall walker.
  if (GetStereoMode() == StereoMode::SameFrameReplayDual) {
    const int ph = g_mode31Phase.load();
    if (ph == static_cast<int>(Mode31Phase::Count)) {
      if (!CallDrawWalkOnceGuarded(self, edx)) {
        Log("Mode31: EXCEPTION in count passthrough code=0x%08X — reject cand",
            g_execViewExcCode);
        g_drawWalkEntries.store(0xFFFFFFFFu);
      }
      return;
    }
    if (ph == static_cast<int>(Mode31Phase::Dual) && !g_drawWalkDead.load() &&
        !g_inDrawWalkDual.load() && IsCamMatrixOverrideEnabled() && g_device && g_holdL &&
        g_holdR) {
      g_inDrawWalkDual.store(true);
      const bool ok = RunMode31DualGuarded(self, edx);
      g_inDrawWalkDual.store(false);
      if (!ok) {
        g_drawWalkDead.store(true);
        g_haveL = g_haveR = false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        Log("Mode31: EXCEPTION dual stage=%ld code=0x%08X @exeRva=0x%X", g_execViewStage,
            g_execViewExcCode, static_cast<unsigned>(g_execViewExcAddr - base));
        Mode31FallbackPairHold("SEH in dual pass");
        CallDrawWalkOnceGuarded(self, edx);
        return;
      }
      const uint32_t n = ++g_mode31DualN;
      if (n <= 6 || (n % 300) == 0)
        Log("Mode31: SAME-FRAME #%u haveL=%d haveR=%d vsPatch=%u vsCallsR=%u sep=%.0fcm "
            "fnRva=0x%X",
            n, g_haveL ? 1 : 0, g_haveR ? 1 : 0, StereoVsTranslateCount(),
            StereoVsRightPassCalls(), GetStereoSepMeters() * 100.f, g_mode31ActiveRva);
      return;
    }
    if (!CallDrawWalkOnceGuarded(self, edx)) {
      Log("Mode31: EXCEPTION idle passthrough — disabling walker hook");
      Mode31FallbackPairHold("SEH in idle walker");
    }
    return;
  }

  const int phase = g_wrapPhase.load();
  if (phase != static_cast<int>(WrapPhase::Dual) || g_drawWalkDead.load() ||
      g_inDrawWalkDual.load() || !IsCamMatrixOverrideEnabled() || !g_device || !g_texL ||
      !g_texR) {
    if (!CallDrawWalkOnceGuarded(self, edx)) {
      Log("WrapDual: EXCEPTION in passthrough/count code=0x%08X — reject this cand",
          g_execViewExcCode);
      // EndScene Count handler will advance; avoid hard TemporalOnly on first bad ABI.
      g_drawWalkEntries.store(0xFFFFFFFFu);
    }
    return;
  }
  g_inDrawWalkDual.store(true);
  const bool ok = RunDrawWalkDualGuarded(self, edx);
  g_inDrawWalkDual.store(false);
  if (!ok) {
    g_drawWalkDead.store(true);
    g_wrapPhase.store(static_cast<int>(WrapPhase::TemporalOnly));
    g_haveL = g_haveR = false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    Log("WrapDual: EXCEPTION dual stage=%ld code=0x%08X @exeRva=0x%X — fallback temporal",
        g_execViewStage, g_execViewExcCode,
        static_cast<unsigned>(g_execViewExcAddr - base));
    CallDrawWalkOnceGuarded(self, edx);
    return;
  }
  const uint32_t n = ++g_drawWalkDualCount;
  if (n <= 6 || (n % 300) == 0)
    Log("WrapDual: SAME-FRAME #%u haveL=%d haveR=%d vsPatch=%u vsCallsR=%u sep=%.0fcm", n,
        g_haveL ? 1 : 0, g_haveR ? 1 : 0, StereoVsTranslateCount(), StereoVsRightPassCalls(),
        GetStereoSepMeters() * 100.f);
}

bool InstallDrawWalkAt(uintptr_t start) {
  if (!start)
    return false;
  if (g_drawWalkAddr) {
    MH_DisableHook(g_drawWalkAddr);
    MH_RemoveHook(g_drawWalkAddr);
    g_origDrawWalk = nullptr;
    g_drawWalkAddr = nullptr;
  }
  if (MH_CreateHook(reinterpret_cast<void*>(start), reinterpret_cast<void*>(&HookDrawWalk),
                    reinterpret_cast<void**>(&g_origDrawWalk)) != MH_OK ||
      MH_EnableHook(reinterpret_cast<void*>(start)) != MH_OK) {
    Log("WrapDual: draw-walk MH FAIL @ %p", reinterpret_cast<void*>(start));
    return false;
  }
  g_drawWalkAddr = reinterpret_cast<void*>(start);
  g_drawWalkEntries.store(0);
  return true;
}

void BuildWrapTryList() {
  g_wrapTryN = 0;
  g_wrapTryIdx = 0;
  // Score: deeper stack (outer) + more hits. Cap 8 candidates.
  int order[32];
  int n = 0;
  for (int i = 0; i < g_wrapFnN; ++i) {
    if (g_wrapFns[i].hits < 50)
      continue;
    order[n++] = i;
  }
  for (int a = 0; a < n; ++a) {
    for (int b = a + 1; b < n; ++b) {
      const int sa = g_wrapFns[order[a]].depth * 100000 + (int)g_wrapFns[order[a]].hits;
      const int sb = g_wrapFns[order[b]].depth * 100000 + (int)g_wrapFns[order[b]].hits;
      if (sb > sa) {
        const int t = order[a];
        order[a] = order[b];
        order[b] = t;
      }
    }
  }
  for (int i = 0; i < n && g_wrapTryN < 8; ++i)
    g_wrapTryList[g_wrapTryN++] = g_wrapFns[order[i]].start;

  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  Log("WrapDual: learn done fnCands=%d tryN=%d wrapCalls=%u", g_wrapFnN, g_wrapTryN,
      g_wrapCalls.load());
  for (int i = 0; i < g_wrapTryN; ++i)
    Log("WrapDual: try[%d] exeRva=0x%X", i, static_cast<unsigned>(g_wrapTryList[i] - base));
}

bool StartNextWrapCount() {
  while (g_wrapTryIdx < g_wrapTryN) {
    const uintptr_t start = g_wrapTryList[g_wrapTryIdx++];
    if (!InstallDrawWalkAt(start))
      continue;
    g_wrapPhase.store(static_cast<int>(WrapPhase::Count));
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    Log("WrapDual: COUNT phase on exeRva=0x%X (want ~1 entry/EndScene)",
        static_cast<unsigned>(start - base));
    return true;
  }
  g_wrapPhase.store(static_cast<int>(WrapPhase::TemporalOnly));
  Log("WrapDual: no ~1×/frame walker — staying on temporal Mode26 path");
  return false;
}

// ---- Mode 31 helpers (safer discovery than Mode 27/29 FindFnStartNear blind) ----
constexpr uint32_t kForbiddenHookRvaA = 0x2C6AC;  // VS wrap — Mode 28 crash
constexpr uint32_t kForbiddenHookRvaB = 0x37BD0;  // wrong-ABI — Mode 27/29 crash
constexpr uint32_t kForbiddenMidRva = 0x37C01;    // resolves to 0x37BD0
constexpr uint32_t kMode31DiscoverEs = 90;
constexpr uint32_t kMode31CountEsNeed = 45;

bool Mode31ForbiddenRva(uint32_t rva) {
  return rva == kForbiddenHookRvaA || rva == kForbiddenHookRvaB || rva == kForbiddenMidRva ||
         rva == 0x2C73E;  // VsRet itself — hundreds/frame
}

// Accept only proven thiscall shapes (mov reg, ecx). Reject Mode 29's 8B FA.
bool Mode31SafeThiscallPrologue(uintptr_t addr) {
  if (!addr)
    return false;
  const auto* b = reinterpret_cast<const uint8_t*>(addr);
  // 56 57 8B F1/F9
  if (b[0] == 0x56 && b[1] == 0x57 && b[2] == 0x8B && (b[3] == 0xF1 || b[3] == 0xF9))
    return true;
  // 53 56 57 8B F1/F9
  if (b[0] == 0x53 && b[1] == 0x56 && b[2] == 0x57 && b[3] == 0x8B &&
      (b[4] == 0xF1 || b[4] == 0xF9))
    return true;
  // 53 55 56 57 8B F1/F9 (ExecRoot-like)
  if (b[0] == 0x53 && b[1] == 0x55 && b[2] == 0x56 && b[3] == 0x57 && b[4] == 0x8B &&
      (b[5] == 0xF1 || b[5] == 0xF9))
    return true;
  // 83 EC xx … 8B F1/F9 within first 12 bytes
  if (b[0] == 0x83 && b[1] == 0xEC) {
    for (int i = 2; i < 11; ++i) {
      if (b[i] == 0x8B && (b[i + 1] == 0xF1 || b[i + 1] == 0xF9))
        return true;
    }
  }
  // 55 8B EC … 8B F1/F9
  if (b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC) {
    for (int i = 3; i < 11; ++i) {
      if (b[i] == 0x8B && (b[i + 1] == 0xF1 || b[i + 1] == 0xF9))
        return true;
    }
  }
  return false;
}

void Mode31FallbackPairHold(const char* why) {
  g_mode31Phase.store(static_cast<int>(Mode31Phase::FallbackPairHold));
  g_vsPatchOn.store(false);
  g_drawWalkDead.store(true);
  SetStereoEye(StereoEye::Left);
  g_pairAwaitingR = false;
  if (g_drawWalkAddr) {
    MH_DisableHook(g_drawWalkAddr);
    MH_RemoveHook(g_drawWalkAddr);
    g_origDrawWalk = nullptr;
    g_drawWalkAddr = nullptr;
  }
  Log("Mode31: FALLBACK pair-hold (Mode30 path) — %s (kill → stereo 30 or 26)", why);
}

void Mode31NoteFrameRva(uint32_t rva) {
  if (Mode31ForbiddenRva(rva))
    return;
  for (int i = 0; i < g_mode31FrameN; ++i) {
    if (g_mode31Frame[i].rva == rva) {
      ++g_mode31Frame[i].count;
      return;
    }
  }
  if (g_mode31FrameN >= 96)
    return;
  g_mode31Frame[g_mode31FrameN++] = {rva, 1};
}

void Mode31AggRare(uint32_t rva, uint32_t hits) {
  for (int i = 0; i < g_mode31AggN; ++i) {
    if (g_mode31Agg[i].rva == rva) {
      g_mode31Agg[i].totalHits += hits;
      ++g_mode31Agg[i].rareFrames;
      return;
    }
  }
  if (g_mode31AggN >= 64)
    return;
  g_mode31Agg[g_mode31AggN++] = {rva, 1, hits};
}

bool RunMode31DualGuarded(void* self, void* edx) {
  __try {
    g_execViewStage = 1;
    float dx = 0.f, dy = 0.f, dz = 0.f;
    float cx = 0.f, cy = 0.f, cz = 0.f;
    const bool sane = GetStereoEyeRightDeltaWorld(&dx, &dy, &dz) &&
                      GetLastStereoCamPos(&cx, &cy, &cz);
    // CCam stays Left both passes — VS translate ONLY (no CCam+VS stack).
    g_vsPatchOn.store(false);
    SetStereoEye(StereoEye::Left);
    g_execViewStage = 2;
    g_origDrawWalk(self, edx);
    g_execViewStage = 3;
    if (g_device && g_holdL && CopyBbToEyeCanvas(g_device, g_holdL, vr::Eye_Left))
      g_pairAwaitingR = true;

    if (!sane) {
      return true;  // keep last submit pair; try again next tick
    }

    g_vsPatchCam[0] = cx;
    g_vsPatchCam[1] = cy;
    g_vsPatchCam[2] = cz;
    g_vsPatchDelta[0] = dx;
    g_vsPatchDelta[1] = dy;
    g_vsPatchDelta[2] = dz;
    g_vsPatchOn.store(true);
    g_execViewStage = 5;
    g_origDrawWalk(self, edx);
    g_vsPatchOn.store(false);
    g_execViewStage = 6;
    if (g_device && g_holdR && CopyBbToEyeCanvas(g_device, g_holdR, vr::Eye_Right) &&
        g_pairAwaitingR) {
      PromoteHoldPair(g_device);
      g_pairAwaitingR = false;
    }
    g_execViewStage = 7;
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    g_vsPatchOn.store(false);
    return false;
  }
}

bool Mode31StartNextCount() {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  while (g_mode31TryIdx < g_mode31TryN) {
    const uintptr_t start = g_mode31TryList[g_mode31TryIdx++];
    const uint32_t rva = static_cast<uint32_t>(start - base);
    if (Mode31ForbiddenRva(rva) || !Mode31SafeThiscallPrologue(start)) {
      Log("Mode31: skip try exeRva=0x%X (forbidden or unsafe prologue)", rva);
      continue;
    }
    if (!InstallDrawWalkAt(start))
      continue;
    g_mode31ActiveRva = rva;
    g_mode31CountEs.store(0);
    g_mode31EntrySum = 0;
    g_drawWalkEntries.store(0);
    g_drawWalkDead.store(false);
    g_mode31Phase.store(static_cast<int>(Mode31Phase::Count));
    const auto* b = reinterpret_cast<const uint8_t*>(start);
    Log("Mode31: COUNT exeRva=0x%X bytes=%02X %02X %02X %02X %02X %02X (need avg∈[0.8,4] "
        "over %u EndScenes)",
        rva, b[0], b[1], b[2], b[3], b[4], b[5], kMode31CountEsNeed);
    return true;
  }
  Mode31FallbackPairHold("no safe ~1×/frame walker after discover+count");
  return false;
}

void Mode31SeedKnownMidRvas();

void Mode31BuildTryListFromDiscover() {
  g_mode31TryN = 0;
  g_mode31TryIdx = 0;
  // Sort agg by rareFrames desc.
  int order[64];
  int n = 0;
  for (int i = 0; i < g_mode31AggN; ++i) {
    if (g_mode31Agg[i].rareFrames < 8)
      continue;
    order[n++] = i;
  }
  for (int a = 0; a < n; ++a) {
    for (int b = a + 1; b < n; ++b) {
      if (g_mode31Agg[order[b]].rareFrames > g_mode31Agg[order[a]].rareFrames) {
        const int t = order[a];
        order[a] = order[b];
        order[b] = t;
      }
    }
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  Log("Mode31: discover done aggN=%d rareCands=%d vsTid=%u vsTotal=%u — resolving",
      g_mode31AggN, n, g_mode31VsTid, StereoVsTotalCalls());
  for (int i = 0; i < n && i < 16; ++i) {
    const Mode31Agg& a = g_mode31Agg[order[i]];
    Log("Mode31: rare cand midRva=0x%X rareFrames=%u totalHits=%u", a.rva, a.rareFrames,
        a.totalHits);
  }
  for (int i = 0; i < n && g_mode31TryN < 8; ++i) {
    const uint32_t midRva = g_mode31Agg[order[i]].rva;
    if (Mode31ForbiddenRva(midRva))
      continue;
    const uintptr_t mid = base + midRva;
    const uintptr_t fn = FindFnStartNear(mid);
    if (!fn) {
      Log("Mode31: midRva=0x%X — no prologue near", midRva);
      continue;
    }
    const uint32_t fnRva = static_cast<uint32_t>(fn - base);
    if (Mode31ForbiddenRva(fnRva)) {
      Log("Mode31: midRva=0x%X -> fnRva=0x%X FORBIDDEN — skip", midRva, fnRva);
      continue;
    }
    if (!Mode31SafeThiscallPrologue(fn)) {
      const auto* b = reinterpret_cast<const uint8_t*>(fn);
      Log("Mode31: midRva=0x%X -> fnRva=0x%X unsafe bytes=%02X %02X %02X %02X — skip",
          midRva, fnRva, b[0], b[1], b[2], b[3]);
      continue;
    }
    bool dup = false;
    for (int j = 0; j < g_mode31TryN; ++j) {
      if (g_mode31TryList[j] == fn) {
        dup = true;
        break;
      }
    }
    if (dup)
      continue;
    g_mode31TryList[g_mode31TryN++] = fn;
    const auto* b = reinterpret_cast<const uint8_t*>(fn);
    Log("Mode31: try[%d] midRva=0x%X -> fnRva=0x%X bytes=%02X %02X %02X %02X %02X %02X",
        g_mode31TryN - 1, midRva, fnRva, b[0], b[1], b[2], b[3], b[4], b[5]);
  }
  if (g_mode31TryN == 0) {
    Log("Mode31: live hist empty/unsafe — seeding Mode26 VsParent mid-RVAs (no 0x37C01)");
    Mode31SeedKnownMidRvas();
  }
  if (g_mode31TryN == 0) {
    Mode31FallbackPairHold("discover+seed found no safe thiscall prologue");
    // Still log top mid RVAs for CURRENT-STATE next session.
    for (int i = 0; i < n && i < 8; ++i) {
      Log("Mode31: NEXT-RVA midRva=0x%X rareFrames=%u (no safe hook this session)",
          g_mode31Agg[order[i]].rva, g_mode31Agg[order[i]].rareFrames);
    }
    return;
  }
  Mode31StartNextCount();
}

void Mode31SeedKnownMidRvas() {
  static const uint32_t kSeed[] = {0x22187, 0x37520, 0x32BD7, 0x4D901B};
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  for (uint32_t midRva : kSeed) {
    if (Mode31ForbiddenRva(midRva))
      continue;
    const uintptr_t fn = FindFnStartNear(base + midRva);
    if (!fn)
      continue;
    const uint32_t fnRva = static_cast<uint32_t>(fn - base);
    if (Mode31ForbiddenRva(fnRva) || !Mode31SafeThiscallPrologue(fn)) {
      const auto* b = reinterpret_cast<const uint8_t*>(fn);
      Log("Mode31: seed midRva=0x%X -> fnRva=0x%X REJECT bytes=%02X %02X %02X %02X",
          midRva, fnRva, b[0], b[1], b[2], b[3]);
      continue;
    }
    bool dup = false;
    for (int j = 0; j < g_mode31TryN; ++j) {
      if (g_mode31TryList[j] == fn) {
        dup = true;
        break;
      }
    }
    if (dup || g_mode31TryN >= 8)
      continue;
    g_mode31TryList[g_mode31TryN++] = fn;
    const auto* b = reinterpret_cast<const uint8_t*>(fn);
    Log("Mode31: seed try[%d] midRva=0x%X -> fnRva=0x%X bytes=%02X %02X %02X %02X",
        g_mode31TryN - 1, midRva, fnRva, b[0], b[1], b[2], b[3]);
  }
}

void Mode31FinishDiscoverFrame(uint32_t es) {
  const uint32_t vsCalls = g_mode31VsCallsFrame.exchange(0);
  const unsigned vsTotal = StereoVsTotalCalls();
  static unsigned s_prevTotal = 0;
  const unsigned vsDelta = vsTotal - s_prevTotal;
  s_prevTotal = vsTotal;
  for (int i = 0; i < g_mode31FrameN; ++i) {
    const uint32_t c = g_mode31Frame[i].count;
    if (c >= 1 && c <= 16)
      Mode31AggRare(g_mode31Frame[i].rva, c);
  }
  g_mode31FrameN = 0;
  if (es <= 6 || (es % 30) == 0)
    Log("Mode31: discover es#%u onVs=%u vsHookDelta=%u rareAgg=%d tid=%u "
        "(want stack RVAs with 1..16 hits/frame)",
        es, vsCalls, vsDelta, g_mode31AggN, g_mode31VsTid);
}

// ---- Mode 32 helpers (HookSetVSConstF-depth discover + ABI gate) ----
constexpr uint32_t kMode32DiscoverEs = 90;
constexpr uint32_t kMode32CountEsNeed = 45;

bool Mode32ForbiddenRva(uint32_t rva) {
  return rva == kForbiddenHookRvaA || rva == kForbiddenHookRvaB || rva == kForbiddenMidRva ||
         rva == 0x2C73E;
}

bool Mode32LooksLikeMovFromEcx(uint8_t b0, uint8_t b1) {
  // 8B F1 / 8B F9 / 8B D9 / 8B C1 / 8B E9 / 8B D1 — mov r32, ecx
  if (b0 != 0x8B)
    return false;
  return b1 == 0xF1 || b1 == 0xF9 || b1 == 0xD9 || b1 == 0xC1 || b1 == 0xE9 || b1 == 0xD1;
}

bool Mode32LooksLikeMovFromEdx(uint8_t b0, uint8_t b1) {
  // 8B F2 / 8B FA / 8B DA / 8B C2 — mov r32, edx (fastcall 2nd arg)
  if (b0 != 0x8B)
    return false;
  return b1 == 0xF2 || b1 == 0xFA || b1 == 0xDA || b1 == 0xC2;
}

Mode32Abi Mode32ClassifyPrologue(uintptr_t addr, const char** tagOut) {
  if (!addr) {
    if (tagOut)
      *tagOut = "null";
    return Mode32Abi::Reject;
  }
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) ||
      mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                                  PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
    if (tagOut)
      *tagOut = "vq-fail";
    return Mode32Abi::Reject;
  }
  const auto* b = reinterpret_cast<const uint8_t*>(addr);
  // Prefer real function starts (INT3 align). Mid-fn hooks AV hard (0x372B0-ish).
  const bool padded = (addr >= 4) && b[-1] == 0xCC && b[-2] == 0xCC;
  auto hasEspArg = [&](int from, int to) {
    for (int i = from; i < to; ++i) {
      // mov r32, [esp+disp] / mov r32, [esp]
      if (b[i] == 0x8B && (b[i + 1] == 0x44 || b[i + 1] == 0x4C || b[i + 1] == 0x54 ||
                           b[i + 1] == 0x5C || b[i + 1] == 0x6C || b[i + 1] == 0x74 ||
                           b[i + 1] == 0x7C) && b[i + 2] == 0x24)
        return true;
      if (b[i] == 0x8B && (b[i + 1] == 0x04 || b[i + 1] == 0x0C || b[i + 1] == 0x14 ||
                           b[i + 1] == 0x1C) && b[i + 2] == 0x24)
        return true;
    }
    return false;
  };

  // push esi; mov r32, ecx — common zero-arg thiscall (e.g. exeRva 0x4DDAD0).
  // Must be checked BEFORE 56 57 … (56 alone is a valid start).
  if (b[0] == 0x56 && Mode32LooksLikeMovFromEcx(b[1], b[2]) &&
      !(b[1] == 0x57)) {
    if (hasEspArg(3, 20)) {
      if (tagOut)
        *tagOut = "thiscall-stackarg";
      return Mode32Abi::Reject;
    }
    if (!padded) {
      if (tagOut)
        *tagOut = "thiscall-unaligned";
      return Mode32Abi::Reject;
    }
    if (tagOut)
      *tagOut = "thiscall-56";
    return Mode32Abi::Thiscall;
  }
  // Known Mode 31 thiscall shapes (tight).
  if (b[0] == 0x56 && b[1] == 0x57 && Mode32LooksLikeMovFromEcx(b[2], b[3])) {
    if (hasEspArg(4, 20)) {
      if (tagOut)
        *tagOut = "thiscall-stackarg";
      return Mode32Abi::Reject;
    }
    if (!padded) {
      if (tagOut)
        *tagOut = "thiscall-unaligned";
      return Mode32Abi::Reject;
    }
    if (tagOut)
      *tagOut = "thiscall-5657";
    return Mode32Abi::Thiscall;
  }
  if (b[0] == 0x53 && b[1] == 0x56 && b[2] == 0x57 && Mode32LooksLikeMovFromEcx(b[3], b[4])) {
    if (hasEspArg(5, 22) || !padded) {
      if (tagOut)
        *tagOut = !padded ? "thiscall-unaligned" : "thiscall-stackarg";
      return Mode32Abi::Reject;
    }
    if (tagOut)
      *tagOut = "thiscall-535657";
    return Mode32Abi::Thiscall;
  }
  if (b[0] == 0x53 && b[1] == 0x55 && b[2] == 0x56 && b[3] == 0x57 &&
      Mode32LooksLikeMovFromEcx(b[4], b[5])) {
    if (hasEspArg(6, 24) || !padded) {
      if (tagOut)
        *tagOut = !padded ? "thiscall-unaligned" : "thiscall-stackarg";
      return Mode32Abi::Reject;
    }
    if (tagOut)
      *tagOut = "thiscall-53555657";
    return Mode32Abi::Thiscall;
  }
  // 83 EC xx … mov r,ecx within 16 bytes
  if (b[0] == 0x83 && b[1] == 0xEC) {
    bool ecx = false, edx = false;
    for (int i = 2; i < 15; ++i) {
      if (Mode32LooksLikeMovFromEcx(b[i], b[i + 1]))
        ecx = true;
      if (Mode32LooksLikeMovFromEdx(b[i], b[i + 1]))
        edx = true;
    }
    if (hasEspArg(2, 20)) {
      if (tagOut)
        *tagOut = "thiscall-stackarg";
      return Mode32Abi::Reject;
    }
    if (!padded) {
      if (tagOut)
        *tagOut = "thiscall-unaligned";
      return Mode32Abi::Reject;
    }
    if (ecx && edx) {
      if (tagOut)
        *tagOut = "fastcall-83EC";
      return Mode32Abi::Fastcall;
    }
    if (ecx) {
      if (tagOut)
        *tagOut = "thiscall-83EC";
      return Mode32Abi::Thiscall;
    }
  }
  // 55 8B EC … widen window to 24 for mov r,ecx (Mode31 missed 0x32A40)
  if (b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC) {
    bool ecx = false, edx = false, stackArg = false;
    for (int i = 3; i < 23; ++i) {
      if (Mode32LooksLikeMovFromEcx(b[i], b[i + 1]))
        ecx = true;
      if (Mode32LooksLikeMovFromEdx(b[i], b[i + 1]))
        edx = true;
      // mov r32, [ebp+8..] = at least one stack arg → unsafe for zero-arg dual
      if (b[i] == 0x8B && (b[i + 1] == 0x45 || b[i + 1] == 0x4D || b[i + 1] == 0x55 ||
                           b[i + 1] == 0x5D) && b[i + 2] >= 0x08)
        stackArg = true;
      // SSE: movss/movsd xmm, [ebp+8..] — Mode34 0x1BF010 hard-kill (missed by GPR scan)
      if ((b[i] == 0xF3 || b[i] == 0xF2) && b[i + 1] == 0x0F &&
          (b[i + 2] == 0x10 || b[i + 2] == 0x11) &&
          (b[i + 3] == 0x45 || b[i + 3] == 0x4D || b[i + 3] == 0x55 || b[i + 3] == 0x5D) &&
          b[i + 4] >= 0x08)
        stackArg = true;
      // ecx used as memory base without mov r,ecx (e.g. F3 0F 10 01 = movss xmm0,[ecx])
      if (b[i] == 0x0F && b[i + 1] == 0x10 && (b[i + 2] & 0xC7) == 0x01)
        ecx = true;
      if (b[i] == 0x8B && (b[i + 1] & 0xC7) == 0x01)
        ecx = true;
    }
    if (stackArg) {
      if (tagOut)
        *tagOut = "thiscall-stackarg";
      return Mode32Abi::Reject;  // needs stdcall-ish re-call; do not HookDrawWalk
    }
    if (ecx && edx) {
      if (tagOut)
        *tagOut = "fastcall-frame";
      return Mode32Abi::Fastcall;
    }
    if (ecx) {
      if (tagOut)
        *tagOut = "thiscall-frame";
      return Mode32Abi::Thiscall;
    }
    if (tagOut)
      *tagOut = "stdcall-frame";
    return Mode32Abi::Stdcall;
  }
  // 56 57 8B 7C/74 = push esi/edi; mov r32, [esp+…] — stack-arg, not thiscall.
  if (b[0] == 0x56 && b[1] == 0x57 && b[2] == 0x8B && (b[3] == 0x7C || b[3] == 0x74)) {
    if (tagOut)
      *tagOut = "stdcall-5657mem";
    return Mode32Abi::Stdcall;
  }
  if (tagOut)
    *tagOut = "unknown";
  return Mode32Abi::Reject;
}

void Mode32NoteFrameRva(uint32_t rva) {
  if (Mode32ForbiddenRva(rva))
    return;
  for (int i = 0; i < g_mode32FrameN; ++i) {
    if (g_mode32Frame[i].rva == rva) {
      ++g_mode32Frame[i].count;
      return;
    }
  }
  if (g_mode32FrameN >= 96)
    return;
  g_mode32Frame[g_mode32FrameN++] = {rva, 1};
}

void Mode32AggRare(uint32_t rva, uint32_t hits) {
  for (int i = 0; i < g_mode32AggN; ++i) {
    if (g_mode32Agg[i].rva == rva) {
      g_mode32Agg[i].totalHits += hits;
      ++g_mode32Agg[i].rareFrames;
      return;
    }
  }
  if (g_mode32AggN >= 64)
    return;
  g_mode32Agg[g_mode32AggN++] = {rva, 1, hits};
}

void Mode32FallbackPairHold(const char* why) {
  g_mode32Phase.store(static_cast<int>(Mode32Phase::FallbackPairHold));
  g_vsPatchOn.store(false);
  g_drawWalkDead.store(true);
  SetStereoEye(StereoEye::Left);
  g_pairAwaitingR = false;
  if (g_drawWalkAddr) {
    MH_DisableHook(g_drawWalkAddr);
    MH_RemoveHook(g_drawWalkAddr);
    g_origDrawWalk = nullptr;
    g_drawWalkAddr = nullptr;
  }
  WriteStereoModeFile(30);
  Log("Mode32: FALLBACK pair-hold — wrote stereo=30 — %s (kill → 30 or 26)", why);
}

bool RunMode32DualGuarded(void* self, void* edx) {
  __try {
    g_execViewStage = 1;
    float dx = 0.f, dy = 0.f, dz = 0.f;
    float cx = 0.f, cy = 0.f, cz = 0.f;
    const bool sane = GetStereoEyeRightDeltaWorld(&dx, &dy, &dz) &&
                      GetLastStereoCamPos(&cx, &cy, &cz);
    g_vsPatchOn.store(false);
    SetStereoEye(StereoEye::Left);
    g_execViewStage = 2;
    g_origDrawWalk(self, edx);
    g_execViewStage = 3;
    if (g_device && g_holdL && CopyBbToEyeCanvas(g_device, g_holdL, vr::Eye_Left))
      g_pairAwaitingR = true;

    if (!sane)
      return true;

    g_vsPatchCam[0] = cx;
    g_vsPatchCam[1] = cy;
    g_vsPatchCam[2] = cz;
    g_vsPatchDelta[0] = dx;
    g_vsPatchDelta[1] = dy;
    g_vsPatchDelta[2] = dz;
    g_vsPatchOn.store(true);
    g_execViewStage = 5;
    g_origDrawWalk(self, edx);
    g_vsPatchOn.store(false);
    g_execViewStage = 6;
    if (g_device && g_holdR && CopyBbToEyeCanvas(g_device, g_holdR, vr::Eye_Right) &&
        g_pairAwaitingR) {
      PromoteHoldPair(g_device);
      g_pairAwaitingR = false;
    }
    g_execViewStage = 7;
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    g_vsPatchOn.store(false);
    return false;
  }
}

bool Mode32StartNextCount() {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  while (g_mode32TryIdx < g_mode32TryN) {
    const uintptr_t start = g_mode32TryList[g_mode32TryIdx++];
    const uint32_t rva = static_cast<uint32_t>(start - base);
    const char* tag = "?";
    const Mode32Abi abi = Mode32ClassifyPrologue(start, &tag);
    if (Mode32ForbiddenRva(rva) || abi != Mode32Abi::Thiscall) {
      Log("Mode32: skip try exeRva=0x%X abi=%s (need thiscall for dual)", rva, tag);
      continue;
    }
    if (!InstallDrawWalkAt(start))
      continue;
    g_mode32ActiveRva = rva;
    g_mode32CountEs.store(0);
    g_mode32EntrySum = 0;
    g_drawWalkEntries.store(0);
    g_drawWalkDead.store(false);
    g_mode32Phase.store(static_cast<int>(Mode32Phase::Count));
    const auto* b = reinterpret_cast<const uint8_t*>(start);
    Log("Mode32: COUNT exeRva=0x%X abi=%s bytes=%02X %02X %02X %02X %02X %02X "
        "(need avg in [0.8,4] over %u EndScenes)",
        rva, tag, b[0], b[1], b[2], b[3], b[4], b[5], kMode32CountEsNeed);
    return true;
  }
  Mode32FallbackPairHold("no safe ~1×/frame thiscall walker after discover+count");
  return false;
}

void Mode32SeedKnownMidRvas() {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  auto tryAdd = [&](uintptr_t fn, uint32_t midRva, const char* how) {
    if (!fn || g_mode32TryN >= 8)
      return;
    const uint32_t fnRva = static_cast<uint32_t>(fn - base);
    const char* tag = "?";
    const Mode32Abi abi = Mode32ClassifyPrologue(fn, &tag);
    if (Mode32ForbiddenRva(fnRva) || abi != Mode32Abi::Thiscall) {
      const auto* b = reinterpret_cast<const uint8_t*>(fn);
      Log("Mode32: seed %s midRva=0x%X -> fnRva=0x%X REJECT abi=%s bytes=%02X %02X %02X %02X",
          how, midRva, fnRva, tag, b[0], b[1], b[2], b[3]);
      return;
    }
    for (int j = 0; j < g_mode32TryN; ++j) {
      if (g_mode32TryList[j] == fn)
        return;
    }
    g_mode32TryList[g_mode32TryN++] = fn;
    Log("Mode32: seed try[%d] %s midRva=0x%X -> fnRva=0x%X abi=%s", g_mode32TryN - 1, how,
        midRva, fnRva, tag);
  };

  // Mode 26 VsParent mid-RVAs → prefer NEAR thiscall (≤0x300), not blind FindFnStartNear.
  static const uint32_t kSeedMid[] = {0x22187, 0x37520, 0x32BD7, 0x4D901B};
  for (uint32_t midRva : kSeedMid) {
    if (Mode32ForbiddenRva(midRva))
      continue;
    const uintptr_t mid = base + midRva;
    uintptr_t fn = FindThiscallNear(mid, 0x300);
    if (!fn)
      fn = FindThiscallNear(mid, 0x800);
    if (!fn) {
      const uintptr_t blind = FindFnStartNear(mid);
      if (blind) {
        const char* tag = "?";
        Mode32ClassifyPrologue(blind, &tag);
        Log("Mode32: seed midRva=0x%X blind fnRva=0x%X abi=%s (no near thiscall)", midRva,
            static_cast<unsigned>(blind - base), tag);
      } else {
        Log("Mode32: seed midRva=0x%X — no prologue near", midRva);
      }
      continue;
    }
    tryAdd(fn, midRva, "thiscallNear");
  }

  // PE-confirmed CC-padded thiscall only. 0x4D8F84 unaligned — skip.
  static const uint32_t kDirectFn[] = {
      0x4D8F10,  // CC-pad 83 EC 14 … 8B F1
  };
  for (uint32_t fnRva : kDirectFn) {
    if (Mode32ForbiddenRva(fnRva))
      continue;
    tryAdd(base + fnRva, fnRva, "directFn");
  }
}

void Mode32BuildTryListFromDiscover() {
  g_mode32TryN = 0;
  g_mode32TryIdx = 0;
  int order[64];
  int n = 0;
  for (int i = 0; i < g_mode32AggN; ++i) {
    if (g_mode32Agg[i].rareFrames < 8)
      continue;
    order[n++] = i;
  }
  for (int a = 0; a < n; ++a) {
    for (int b = a + 1; b < n; ++b) {
      if (g_mode32Agg[order[b]].rareFrames > g_mode32Agg[order[a]].rareFrames) {
        const int t = order[a];
        order[a] = order[b];
        order[b] = t;
      }
    }
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  Log("Mode32: discover done aggN=%d rareCands=%d vsTid=%u vsTotal=%u — resolving",
      g_mode32AggN, n, g_mode32VsTid, StereoVsTotalCalls());
  for (int i = 0; i < n && i < 16; ++i) {
    const Mode32Agg& a = g_mode32Agg[order[i]];
    Log("Mode32: rare cand midRva=0x%X rareFrames=%u totalHits=%u", a.rva, a.rareFrames,
        a.totalHits);
  }
  for (int i = 0; i < n && g_mode32TryN < 8; ++i) {
    const uint32_t midRva = g_mode32Agg[order[i]].rva;
    if (Mode32ForbiddenRva(midRva))
      continue;
    const uintptr_t mid = base + midRva;
    uintptr_t fn = FindThiscallNear(mid, 0x300);
    if (!fn)
      fn = FindThiscallNear(mid, 0x800);
    if (!fn) {
      const uintptr_t blind = FindFnStartNear(mid);
      if (blind) {
        const char* tag = "?";
        const Mode32Abi abi = Mode32ClassifyPrologue(blind, &tag);
        Log("Mode32: midRva=0x%X blind fnRva=0x%X abi=%s — skip (want thiscallNear)", midRva,
            static_cast<unsigned>(blind - base), tag);
      } else {
        Log("Mode32: midRva=0x%X — no prologue near", midRva);
      }
      continue;
    }
    const uint32_t fnRva = static_cast<uint32_t>(fn - base);
    const char* tag = "?";
    const Mode32Abi abi = Mode32ClassifyPrologue(fn, &tag);
    const auto* b = reinterpret_cast<const uint8_t*>(fn);
    if (Mode32ForbiddenRva(fnRva)) {
      Log("Mode32: midRva=0x%X -> fnRva=0x%X FORBIDDEN — skip", midRva, fnRva);
      continue;
    }
    if (abi != Mode32Abi::Thiscall) {
      Log("Mode32: midRva=0x%X -> fnRva=0x%X NEXT-RVA abi=%s bytes=%02X %02X %02X %02X "
          "(not thiscall — no hook)",
          midRva, fnRva, tag, b[0], b[1], b[2], b[3]);
      continue;
    }
    bool dup = false;
    for (int j = 0; j < g_mode32TryN; ++j) {
      if (g_mode32TryList[j] == fn) {
        dup = true;
        break;
      }
    }
    if (dup)
      continue;
    g_mode32TryList[g_mode32TryN++] = fn;
    Log("Mode32: try[%d] midRva=0x%X -> fnRva=0x%X abi=%s bytes=%02X %02X %02X %02X %02X %02X",
        g_mode32TryN - 1, midRva, fnRva, tag, b[0], b[1], b[2], b[3], b[4], b[5]);
  }
  if (g_mode32TryN == 0) {
    Log("Mode32: live hist empty/unsafe — seeding Mode26 VsParent mid-RVAs (no 0x37C01)");
    Mode32SeedKnownMidRvas();
  }
  if (g_mode32TryN == 0) {
    Mode32FallbackPairHold("discover+seed found no safe thiscall prologue");
    for (int i = 0; i < n && i < 8; ++i)
      Log("Mode32: NEXT-RVA midRva=0x%X rareFrames=%u (logged for CURRENT-STATE)",
          g_mode32Agg[order[i]].rva, g_mode32Agg[order[i]].rareFrames);
    return;
  }
  Mode32StartNextCount();
}

void Mode32FinishDiscoverFrame(uint32_t es) {
  const uint32_t vsCalls = g_mode32VsCallsFrame.exchange(0);
  const unsigned vsTotal = StereoVsTotalCalls();
  static unsigned s_prevTotal32 = 0;
  const unsigned vsDelta = vsTotal - s_prevTotal32;
  s_prevTotal32 = vsTotal;
  const int slots = g_mode32FrameN;
  for (int i = 0; i < g_mode32FrameN; ++i) {
    const uint32_t c = g_mode32Frame[i].count;
    if (c >= 1 && c <= 16)
      Mode32AggRare(g_mode32Frame[i].rva, c);
  }
  g_mode32FrameN = 0;
  if (es <= 6 || (es % 30) == 0)
    Log("Mode32: discover es#%u onVs=%u vsHookDelta=%u rareAgg=%d frameSlots=%d tid=%u "
        "(HookSetVSConstF-depth VsParent)",
        es, vsCalls, vsDelta, g_mode32AggN, slots, g_mode32VsTid);
}

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

// Mode 23: per-phase double exec (proven crash-free by mode 10) + manager-cam
// matrix shift between the passes (the piece mode 10 was missing). Runs inside
// the first HookExecA of the frame on the render thread.
// Mode 30: same dual + DEVICE VS push before Right (not upload-hook; not prologues).
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
    const StereoMode mode = GetStereoMode();
    // Mode 30: force Left CCam bookkeeping so we do NOT stack CCam+VS (double IPD).
    if (mode == StereoMode::PhaseDualDeviceVs) {
      SetStereoEye(StereoEye::Left);
      g_vsPatchOn.store(false);
    }
    g_execViewStage = 2;  // exec pass 1 (LEFT — CCam snapshot is the left eye)
    RunExecuteEyePass(edx);
    g_execViewStage = 3;  // capture L
    if (CopyCurrentRtToEyeCanvas(g_device, g_texL, vr::Eye_Left))
      g_haveL = true;
    g_execViewStage = 4;  // matrix shift to RIGHT eye
    const float ax = posA[0], ay = posA[1], az = posA[2];
    const float bx = posB[0], by = posB[1], bz = posB[2];
    // Mode 23/24: manager matrices (proven inert for fusion). Mode 30: skip — VS only.
    if (mode != StereoMode::PhaseDualDeviceVs) {
      posA[0] = ax + dx;
      posA[1] = ay + dy;
      posA[2] = az + dz;
      posB[0] = bx + dx;
      posB[1] = by + dy;
      posB[2] = bz + dz;
    }
    if (mode == StereoMode::ExecViewConstDual) {
      g_vsPatchCam[0] = cx;
      g_vsPatchCam[1] = cy;
      g_vsPatchCam[2] = cz;
      g_vsPatchDelta[0] = dx;
      g_vsPatchDelta[1] = dy;
      g_vsPatchDelta[2] = dz;
      g_vsPatchOn.store(true);
    }
    int devPatched = 0;
    if (mode == StereoMode::PhaseDualDeviceVs && g_device) {
      const float cam[3] = {cx, cy, cz};
      const float delta[3] = {dx, dy, dz};
      devPatched = PushDeviceVsEyeTranslate(g_device, cam, delta);
      g_mode30DevPatches.fetch_add(static_cast<uint32_t>(devPatched));
    }
    g_execViewStage = 5;  // exec pass 2
    RunExecuteEyePass(edx);
    g_vsPatchOn.store(false);
    g_execViewStage = 6;  // capture R
    if (CopyCurrentRtToEyeCanvas(g_device, g_texR, vr::Eye_Right))
      g_haveR = true;
    g_execViewStage = 7;  // restore
    if (mode != StereoMode::PhaseDualDeviceVs) {
      posA[0] = ax;
      posA[1] = ay;
      posA[2] = az;
      posB[0] = bx;
      posB[1] = by;
      posB[2] = bz;
    }
    if (mode == StereoMode::PhaseDualDeviceVs && (g_execViewDualCount.load() < 6 ||
                                                   (g_execViewDualCount.load() % 300) == 0))
      Log("StereoMode30: deviceVsPatches=%d sep=%.0fcm (before R pass)", devPatched,
          GetStereoSepMeters() * 100.f);
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    return false;
  }
}

// Definitive answer to "are both eyes the same image?": downsample both canvases
// to 16x16, read back, sum absolute byte differences. 0 = identical (no parallax
// reached the render). Called rarely (GetRenderTargetData syncs the GPU).
long long CompareEyeCanvases(IDirect3DDevice9* dev) {
  if (!dev || !g_texL || !g_texR)
    return -1;
  for (int i = 0; i < 2; ++i) {
    if (!g_diffSmall[i] &&
        FAILED(dev->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                       &g_diffSmall[i], nullptr)))
      return -1;
    if (!g_diffSys[i] &&
        FAILED(dev->CreateOffscreenPlainSurface(16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                &g_diffSys[i], nullptr)))
      return -1;
  }
  for (int i = 0; i < 2; ++i) {
    IDirect3DSurface9* s = nullptr;
    IDirect3DTexture9* t = i ? g_texR : g_texL;
    if (FAILED(t->GetSurfaceLevel(0, &s)) || !s)
      return -1;
    const bool ok = SUCCEEDED(dev->StretchRect(s, nullptr, g_diffSmall[i], nullptr,
                                               D3DTEXF_LINEAR)) &&
                    SUCCEEDED(dev->GetRenderTargetData(g_diffSmall[i], g_diffSys[i]));
    s->Release();
    if (!ok)
      return -1;
  }
  D3DLOCKED_RECT la{}, lb{};
  if (FAILED(g_diffSys[0]->LockRect(&la, nullptr, D3DLOCK_READONLY)))
    return -1;
  if (FAILED(g_diffSys[1]->LockRect(&lb, nullptr, D3DLOCK_READONLY))) {
    g_diffSys[0]->UnlockRect();
    return -1;
  }
  long long sum = 0;
  for (int y = 0; y < 16; ++y) {
    const uint8_t* pa = static_cast<const uint8_t*>(la.pBits) + y * la.Pitch;
    const uint8_t* pb = static_cast<const uint8_t*>(lb.pBits) + y * lb.Pitch;
    for (int x = 0; x < 64; ++x)
      sum += (pa[x] > pb[x]) ? (pa[x] - pb[x]) : (pb[x] - pa[x]);
  }
  g_diffSys[1]->UnlockRect();
  g_diffSys[0]->UnlockRect();
  Log("StereoDiff: L-vs-R 16x16 absdiff=%lld (0 = identical images, no parallax)", sum);
  return sum;
}

void Mode30FallbackPairHold(const char* why) {
  g_execDualDead.store(true);
  g_mode30Phase.store(static_cast<int>(Mode30Phase::PairHold));
  g_vsPatchOn.store(false);
  SetStereoEye(StereoEye::Left);
  g_pairAwaitingR = false;
  Log("StereoMode30: FALLBACK pair-hold temporal — %s (kill switch still stereo=26)", why);
}

void TemporalCapturePairHold(IDirect3DDevice9* device);

void Mode31OnEndScene(IDirect3DDevice9* device) {
  if (!device)
    return;
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    return;
  D3DSURFACE_DESC desc{};
  bb->GetDesc(&desc);
  bb->Release();
  uint32_t rtW = desc.Width, rtH = desc.Height;
  ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
  if (!EnsureEyeRts(device, rtW, rtH))
    return;
  if (!IsCamMatrixOverrideEnabled())
    return;

  const int ph = g_mode31Phase.load();
  const uint32_t es = ++g_mode31Es;

  if (ph == static_cast<int>(Mode31Phase::SoftPairHold) ||
      ph == static_cast<int>(Mode31Phase::FallbackPairHold)) {
    TemporalCapturePairHold(device);
    if (ph == static_cast<int>(Mode31Phase::SoftPairHold))
      Mode31FinishDiscoverFrame(es);
    if (ph == static_cast<int>(Mode31Phase::SoftPairHold) && es >= 45) {
      g_mode31Phase.store(static_cast<int>(Mode31Phase::Discover));
      Log("Mode31: DISCOVER armed (SetVSConstF stack hist on VsRet tid; no 0x2C6AC/0x37BD0; "
          "%u EndScenes) — pair-hold continues; soft already collected rareAgg=%d",
          kMode31DiscoverEs, g_mode31AggN);
    }
    return;
  }

  if (ph == static_cast<int>(Mode31Phase::Discover)) {
    TemporalCapturePairHold(device);
    Mode31FinishDiscoverFrame(es);
    // Discover window counted from phase entry: use agg rareFrames growth via es soft.
    static uint32_t s_discStart = 0;
    if (s_discStart == 0)
      s_discStart = es;
    if (es - s_discStart >= kMode31DiscoverEs) {
      s_discStart = 0;
      Mode31BuildTryListFromDiscover();
    }
    return;
  }

  if (ph == static_cast<int>(Mode31Phase::Count)) {
    TemporalCapturePairHold(device);
    const uint32_t entries = g_drawWalkEntries.exchange(0);
    if (entries == 0xFFFFFFFFu) {
      Log("Mode31: count cand exeRva=0x%X died — try next", g_mode31ActiveRva);
      if (!Mode31StartNextCount())
        return;
      return;
    }
    ++g_mode31CountEs;
    g_mode31EntrySum += entries;
    const uint32_t n = g_mode31CountEs.load();
    if (n <= 6 || (n % 15) == 0)
      Log("Mode31: count es#%u/%u entries=%u sum=%u fnRva=0x%X", n, kMode31CountEsNeed,
          entries, g_mode31EntrySum, g_mode31ActiveRva);
    if (n >= kMode31CountEsNeed) {
      const float avg = static_cast<float>(g_mode31EntrySum) / static_cast<float>(n);
      if (avg >= 0.8f && avg <= 4.f) {
        g_mode31Phase.store(static_cast<int>(Mode31Phase::Dual));
        g_mode31DualN.store(0);
        g_mode31ZeroDiff.store(0);
        g_drawWalkDead.store(false);
        Log("Mode31: DUAL armed fnRva=0x%X avgEntries=%.2f (VS patch between passes, "
            "CCam Left-only, pair-hold promote)",
            g_mode31ActiveRva, avg);
      } else {
        Log("Mode31: count REJECT fnRva=0x%X avgEntries=%.2f (want [0.8,4]) — try next",
            g_mode31ActiveRva, avg);
        if (!Mode31StartNextCount())
          return;
      }
    }
    return;
  }

  if (ph == static_cast<int>(Mode31Phase::Dual)) {
    // Captures happen inside HookDrawWalk dual; optionally verify parallax.
    if (g_haveL && g_haveR && (g_mode31DualN.load() == 5 || g_mode31DualN.load() == 30 ||
                               (g_mode31DualN.load() > 0 && (g_mode31DualN.load() % 300) == 0))) {
      const long long diff = CompareEyeCanvases(device);
      if (diff >= 0 && diff < 200) {
        const uint32_t z = ++g_mode31ZeroDiff;
        if (z >= 3)
          Mode31FallbackPairHold("StereoDiff~0 (dual same cam — VS patch not reaching draws)");
      } else if (diff >= 200) {
        g_mode31ZeroDiff.store(0);
      }
    }
    if (es <= 8 || (es % 300) == 0)
      Log("Mode31: dual EndScene es#%u dualN=%u haveL=%d haveR=%d vsPatch=%u", es,
          g_mode31DualN.load(), g_haveL ? 1 : 0, g_haveR ? 1 : 0, StereoVsTranslateCount());
    return;
  }
}

void Mode32OnEndScene(IDirect3DDevice9* device) {
  if (!device)
    return;
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    return;
  D3DSURFACE_DESC desc{};
  bb->GetDesc(&desc);
  bb->Release();
  uint32_t rtW = desc.Width, rtH = desc.Height;
  ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
  if (!EnsureEyeRts(device, rtW, rtH))
    return;
  if (!IsCamMatrixOverrideEnabled())
    return;

  const int ph = g_mode32Phase.load();
  const uint32_t es = ++g_mode32Es;

  if (ph == static_cast<int>(Mode32Phase::SoftPairHold) ||
      ph == static_cast<int>(Mode32Phase::FallbackPairHold)) {
    TemporalCapturePairHold(device);
    if (ph == static_cast<int>(Mode32Phase::SoftPairHold))
      Mode32FinishDiscoverFrame(es);
    if (ph == static_cast<int>(Mode32Phase::SoftPairHold) && es >= 45) {
      // Mode 31/32 live hist often stays empty (SetVSConstF quiet after cam arm).
      // If soft already has rare parents, do a short Discover; else seed immediately.
      if (g_mode32AggN > 0) {
        g_mode32Phase.store(static_cast<int>(Mode32Phase::Discover));
        Log("Mode32: DISCOVER armed (HookSetVSConstF-depth VsParent; no 0x2C6AC/0x37BD0; "
            "%u EndScenes) — pair-hold continues; soft rareAgg=%d",
            kMode32DiscoverEs, g_mode32AggN);
      } else {
        Log("Mode32: soft rareAgg=0 (vsTotal=%u) — skip empty Discover, seed Mode26 "
            "VsParent mid-RVAs now",
            StereoVsTotalCalls());
        Mode32BuildTryListFromDiscover();
      }
    }
    return;
  }

  if (ph == static_cast<int>(Mode32Phase::Discover)) {
    TemporalCapturePairHold(device);
    Mode32FinishDiscoverFrame(es);
    static uint32_t s_discStart32 = 0;
    if (s_discStart32 == 0)
      s_discStart32 = es;
    if (es - s_discStart32 >= kMode32DiscoverEs) {
      s_discStart32 = 0;
      Mode32BuildTryListFromDiscover();
    }
    return;
  }

  if (ph == static_cast<int>(Mode32Phase::Count)) {
    TemporalCapturePairHold(device);
    const uint32_t entries = g_drawWalkEntries.exchange(0);
    if (entries == 0xFFFFFFFFu) {
      Log("Mode32: count cand exeRva=0x%X died — try next", g_mode32ActiveRva);
      if (!Mode32StartNextCount())
        return;
      return;
    }
    ++g_mode32CountEs;
    g_mode32EntrySum += entries;
    const uint32_t n = g_mode32CountEs.load();
    if (n <= 6 || (n % 15) == 0)
      Log("Mode32: count es#%u/%u entries=%u sum=%u fnRva=0x%X", n, kMode32CountEsNeed,
          entries, g_mode32EntrySum, g_mode32ActiveRva);
    if (n >= kMode32CountEsNeed) {
      const float avg = static_cast<float>(g_mode32EntrySum) / static_cast<float>(n);
      if (avg >= 0.8f && avg <= 4.f) {
        g_mode32Phase.store(static_cast<int>(Mode32Phase::Dual));
        g_mode32DualN.store(0);
        g_mode32ZeroDiff.store(0);
        g_drawWalkDead.store(false);
        Log("Mode32: DUAL armed fnRva=0x%X avgEntries=%.2f (VS patch between passes, "
            "CCam Left-only, pair-hold promote)",
            g_mode32ActiveRva, avg);
      } else {
        Log("Mode32: count REJECT fnRva=0x%X avgEntries=%.2f (want [0.8,4]) — try next",
            g_mode32ActiveRva, avg);
        if (!Mode32StartNextCount())
          return;
      }
    }
    return;
  }

  if (ph == static_cast<int>(Mode32Phase::Dual)) {
    if (g_haveL && g_haveR && (g_mode32DualN.load() == 5 || g_mode32DualN.load() == 30 ||
                               (g_mode32DualN.load() > 0 && (g_mode32DualN.load() % 300) == 0))) {
      const long long diff = CompareEyeCanvases(device);
      if (diff >= 0 && diff < 200) {
        const uint32_t z = ++g_mode32ZeroDiff;
        if (z >= 3)
          Mode32FallbackPairHold("StereoDiff~0 (dual same cam — VS patch not reaching draws)");
      } else if (diff >= 200) {
        g_mode32ZeroDiff.store(0);
      }
    }
    if (es <= 8 || (es % 300) == 0)
      Log("Mode32: dual EndScene es#%u dualN=%u haveL=%d haveR=%d vsPatch=%u", es,
          g_mode32DualN.load(), g_haveL ? 1 : 0, g_haveR ? 1 : 0, StereoVsTranslateCount());
    return;
  }
}

// ---- Mode 33: wait for live VsParent → CC-pad thiscall0 only → dual ----
constexpr uint32_t kMode33SoftWaitMaxEs = 360;   // pair-hold while waiting for samples
constexpr uint32_t kMode33DiscoverEs = 90;
constexpr uint32_t kMode33CountEsNeed = 45;

bool Mode33ForbiddenRva(uint32_t rva) {
  // Mode 32 rejects + known stackarg / crash prologues (never blind-hook).
  return Mode32ForbiddenRva(rva) || rva == 0x32A40 || rva == 0x372B0 || rva == 0x370D0;
}

bool Mode33IsCcPaddedThiscall0(uintptr_t addr, const char** tagOut) {
  const char* tag = "?";
  const Mode32Abi abi = Mode32ClassifyPrologue(addr, &tag);
  if (tagOut)
    *tagOut = tag;
  if (abi != Mode32Abi::Thiscall)
    return false;
  // Mode32Classify already requires CC-pad for most shapes; also reject any
  // thiscall-frame that slipped through without pad (stricter than Mode 32).
  if (addr < 4)
    return false;
  const auto* b = reinterpret_cast<const uint8_t*>(addr);
  if (!(b[-1] == 0xCC && b[-2] == 0xCC)) {
    if (tagOut)
      *tagOut = "thiscall-unpadded";
    return false;
  }
  return true;
}

void Mode33NoteFrameRva(uint32_t rva) {
  if (Mode33ForbiddenRva(rva))
    return;
  ++g_mode33SampleHits;
  for (int i = 0; i < g_mode33FrameN; ++i) {
    if (g_mode33Frame[i].rva == rva) {
      ++g_mode33Frame[i].count;
      return;
    }
  }
  if (g_mode33FrameN >= 96)
    return;
  g_mode33Frame[g_mode33FrameN++] = {rva, 1};
}

void Mode33AggRare(uint32_t rva, uint32_t hits) {
  for (int i = 0; i < g_mode33AggN; ++i) {
    if (g_mode33Agg[i].rva == rva) {
      g_mode33Agg[i].totalHits += hits;
      ++g_mode33Agg[i].rareFrames;
      return;
    }
  }
  if (g_mode33AggN >= 64)
    return;
  g_mode33Agg[g_mode33AggN++] = {rva, 1, hits};
}

void Mode33FallbackPairHold(const char* why) {
  g_mode33Phase.store(static_cast<int>(Mode33Phase::FallbackPairHold));
  g_vsPatchOn.store(false);
  g_drawWalkDead.store(true);
  SetStereoEye(StereoEye::Left);
  g_pairAwaitingR = false;
  if (g_drawWalkAddr) {
    MH_DisableHook(g_drawWalkAddr);
    MH_RemoveHook(g_drawWalkAddr);
    g_origDrawWalk = nullptr;
    g_drawWalkAddr = nullptr;
  }
  WriteStereoModeFile(30);
  Log("Mode33: FALLBACK pair-hold — wrote stereo=30 — %s (kill → 30 or 26)", why);
}

bool RunMode33DualGuarded(void* self, void* edx) {
  // Same dual body as Mode 32 (VS translate pass2, CCam Left-only, pair-hold).
  return RunMode32DualGuarded(self, edx);
}

bool Mode33StartNextCount() {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  while (g_mode33TryIdx < g_mode33TryN) {
    const uintptr_t start = g_mode33TryList[g_mode33TryIdx++];
    const uint32_t rva = static_cast<uint32_t>(start - base);
    const char* tag = "?";
    if (Mode33ForbiddenRva(rva) || !Mode33IsCcPaddedThiscall0(start, &tag)) {
      Log("Mode33: skip try exeRva=0x%X abi=%s (need CC-pad thiscall0)", rva, tag);
      continue;
    }
    if (!InstallDrawWalkAt(start))
      continue;
    g_mode33ActiveRva = rva;
    g_mode33CountEs.store(0);
    g_mode33EntrySum = 0;
    g_drawWalkEntries.store(0);
    g_drawWalkDead.store(false);
    g_mode33Phase.store(static_cast<int>(Mode33Phase::Count));
    const auto* b = reinterpret_cast<const uint8_t*>(start);
    Log("Mode33: COUNT exeRva=0x%X abi=%s bytes=%02X %02X %02X %02X %02X %02X "
        "(need avg in [0.8,4] over %u EndScenes)",
        rva, tag, b[0], b[1], b[2], b[3], b[4], b[5], kMode33CountEsNeed);
    return true;
  }
  Mode33FallbackPairHold("no safe CC-pad thiscall0 walker after live discover");
  return false;
}

void Mode33BuildTryListFromDiscover() {
  g_mode33TryN = 0;
  g_mode33TryIdx = 0;
  int order[64];
  int n = 0;
  for (int i = 0; i < g_mode33AggN; ++i) {
    if (g_mode33Agg[i].rareFrames < 2)
      continue;
    order[n++] = i;
  }
  for (int a = 0; a < n; ++a) {
    for (int b = a + 1; b < n; ++b) {
      if (g_mode33Agg[order[b]].rareFrames > g_mode33Agg[order[a]].rareFrames) {
        const int t = order[a];
        order[a] = order[b];
        order[b] = t;
      }
    }
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  Log("Mode33: discover done aggN=%d rareCands=%d sampleHits=%u vsTid=%u vsTotal=%u",
      g_mode33AggN, n, g_mode33SampleHits, g_mode33VsTid, StereoVsTotalCalls());
  for (int i = 0; i < n && i < 16; ++i) {
    const Mode33Agg& a = g_mode33Agg[order[i]];
    Log("Mode33: rare cand midRva=0x%X rareFrames=%u totalHits=%u", a.rva, a.rareFrames,
        a.totalHits);
  }

  auto tryAdd = [&](uintptr_t fn, uint32_t fromRva, const char* how) {
    if (!fn || g_mode33TryN >= 8)
      return;
    const uint32_t fnRva = static_cast<uint32_t>(fn - base);
    const char* tag = "?";
    if (Mode33ForbiddenRva(fnRva) || !Mode33IsCcPaddedThiscall0(fn, &tag)) {
      const auto* b = reinterpret_cast<const uint8_t*>(fn);
      Log("Mode33: %s midRva=0x%X -> fnRva=0x%X REJECT abi=%s bytes=%02X %02X %02X %02X", how,
          fromRva, fnRva, tag, b[0], b[1], b[2], b[3]);
      return;
    }
    for (int j = 0; j < g_mode33TryN; ++j) {
      if (g_mode33TryList[j] == fn)
        return;
    }
    g_mode33TryList[g_mode33TryN++] = fn;
    const auto* b = reinterpret_cast<const uint8_t*>(fn);
    Log("Mode33: try[%d] %s midRva=0x%X -> fnRva=0x%X abi=%s bytes=%02X %02X %02X %02X %02X %02X",
        g_mode33TryN - 1, how, fromRva, fnRva, tag, b[0], b[1], b[2], b[3], b[4], b[5]);
  };

  for (int i = 0; i < n && g_mode33TryN < 8; ++i) {
    const uint32_t midRva = g_mode33Agg[order[i]].rva;
    if (Mode33ForbiddenRva(midRva))
      continue;
    const uintptr_t mid = base + midRva;
    // If the stack slot itself is already a CC-pad thiscall0 start, hook it.
    tryAdd(mid, midRva, "slotIsStart");
    uintptr_t fn = FindThiscallNear(mid, 0x300);
    if (!fn)
      fn = FindThiscallNear(mid, 0x800);
    if (!fn)
      fn = FindThiscallNear(mid, 0x1000);
    if (fn)
      tryAdd(fn, midRva, "thiscallNear");
    else {
      const uintptr_t blind = FindFnStartNear(mid);
      if (blind)
        tryAdd(blind, midRva, "fnStartNear");
      else
        Log("Mode33: midRva=0x%X — no prologue near (no blind seed)", midRva);
    }
  }

  // Safe PE-confirmed CC-pad thiscall (Mode32 count avg=0 — may still reject after gate).
  tryAdd(base + 0x4D8F10, 0x4D8F10, "directFn");

  if (g_mode33TryN == 0) {
    Mode33FallbackPairHold("live VsParent hist found no CC-pad thiscall0 (no early seed)");
    return;
  }
  Mode33StartNextCount();
}

void Mode33FinishDiscoverFrame(uint32_t es) {
  const uint32_t vsCalls = g_mode33VsCallsFrame.exchange(0);
  const unsigned vsTotal = StereoVsTotalCalls();
  static unsigned s_prevTotal33 = 0;
  const unsigned vsDelta = vsTotal - s_prevTotal33;
  s_prevTotal33 = vsTotal;
  const int slots = g_mode33FrameN;
  for (int i = 0; i < g_mode33FrameN; ++i) {
    const uint32_t c = g_mode33Frame[i].count;
    if (c >= 1 && c <= 16)
      Mode33AggRare(g_mode33Frame[i].rva, c);
  }
  g_mode33FrameN = 0;
  if (es <= 6 || (es % 30) == 0)
    Log("Mode33: wait/discover es#%u onVs=%u vsHookDelta=%u rareAgg=%d frameSlots=%d "
        "sampleHits=%u tid=%u",
        es, vsCalls, vsDelta, g_mode33AggN, slots, g_mode33SampleHits, g_mode33VsTid);
}

void Mode33OnEndScene(IDirect3DDevice9* device) {
  if (!device)
    return;
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    return;
  D3DSURFACE_DESC desc{};
  bb->GetDesc(&desc);
  bb->Release();
  uint32_t rtW = desc.Width, rtH = desc.Height;
  ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
  if (!EnsureEyeRts(device, rtW, rtH))
    return;
  if (!IsCamMatrixOverrideEnabled())
    return;

  const int ph = g_mode33Phase.load();
  const uint32_t es = ++g_mode33Es;

  if (ph == static_cast<int>(Mode33Phase::SoftWaitSamples) ||
      ph == static_cast<int>(Mode33Phase::FallbackPairHold)) {
    TemporalCapturePairHold(device);
    if (ph == static_cast<int>(Mode33Phase::SoftWaitSamples))
      Mode33FinishDiscoverFrame(es);
    if (ph == static_cast<int>(Mode33Phase::SoftWaitSamples)) {
      // KEY vs Mode 32: do NOT seed while hist empty. Wait for live VsParent.
      if (g_mode33AggN > 0 && g_mode33SampleHits >= 8 && es >= 45) {
        g_mode33Phase.store(static_cast<int>(Mode33Phase::Discover));
        Log("Mode33: DISCOVER armed (live VsParent appeared; rareAgg=%d sampleHits=%u; "
            "no early seed; %u more ES) — pair-hold continues",
            g_mode33AggN, g_mode33SampleHits, kMode33DiscoverEs);
      } else if (es >= kMode33SoftWaitMaxEs) {
        Mode33FallbackPairHold("no live VsParent samples after soft wait — keep Mode 30");
      } else if (es >= 45 && (es % 60) == 0) {
        Log("Mode33: still waiting for VsParent samples es=%u rareAgg=%d sampleHits=%u "
            "vsTotal=%u (Mode32 empty Soft was too early)",
            es, g_mode33AggN, g_mode33SampleHits, StereoVsTotalCalls());
      }
    }
    return;
  }

  if (ph == static_cast<int>(Mode33Phase::Discover)) {
    TemporalCapturePairHold(device);
    Mode33FinishDiscoverFrame(es);
    static uint32_t s_discStart33 = 0;
    if (s_discStart33 == 0)
      s_discStart33 = es;
    if (es - s_discStart33 >= kMode33DiscoverEs) {
      s_discStart33 = 0;
      Mode33BuildTryListFromDiscover();
    }
    return;
  }

  if (ph == static_cast<int>(Mode33Phase::Count)) {
    TemporalCapturePairHold(device);
    const uint32_t entries = g_drawWalkEntries.exchange(0);
    if (entries == 0xFFFFFFFFu) {
      Log("Mode33: count cand exeRva=0x%X died — try next", g_mode33ActiveRva);
      if (!Mode33StartNextCount())
        return;
      return;
    }
    ++g_mode33CountEs;
    g_mode33EntrySum += entries;
    const uint32_t n = g_mode33CountEs.load();
    if (n <= 6 || (n % 15) == 0)
      Log("Mode33: count es#%u/%u entries=%u sum=%u fnRva=0x%X", n, kMode33CountEsNeed,
          entries, g_mode33EntrySum, g_mode33ActiveRva);
    if (n >= kMode33CountEsNeed) {
      const float avg = static_cast<float>(g_mode33EntrySum) / static_cast<float>(n);
      if (avg >= 0.8f && avg <= 4.f) {
        g_mode33Phase.store(static_cast<int>(Mode33Phase::Dual));
        g_mode33DualN.store(0);
        g_mode33ZeroDiff.store(0);
        g_drawWalkDead.store(false);
        Log("Mode33: DUAL armed fnRva=0x%X avgEntries=%.2f (VS patch pass2, CCam Left-only, "
            "pair-hold promote)",
            g_mode33ActiveRva, avg);
      } else {
        Log("Mode33: count REJECT fnRva=0x%X avgEntries=%.2f (want [0.8,4]) — try next",
            g_mode33ActiveRva, avg);
        if (!Mode33StartNextCount())
          return;
      }
    }
    return;
  }

  if (ph == static_cast<int>(Mode33Phase::Dual)) {
    if (g_haveL && g_haveR && (g_mode33DualN.load() == 5 || g_mode33DualN.load() == 30 ||
                               (g_mode33DualN.load() > 0 && (g_mode33DualN.load() % 300) == 0))) {
      const long long diff = CompareEyeCanvases(device);
      if (diff >= 0 && diff < 200) {
        const uint32_t z = ++g_mode33ZeroDiff;
        if (z >= 3)
          Mode33FallbackPairHold("StereoDiff~0 (dual same cam — VS patch not reaching draws)");
      } else if (diff >= 200) {
        g_mode33ZeroDiff.store(0);
      }
    }
    if (es <= 8 || (es % 300) == 0)
      Log("Mode33: dual EndScene es#%u dualN=%u haveL=%d haveR=%d vsPatch=%u", es,
          g_mode33DualN.load(), g_haveL ? 1 : 0, g_haveR ? 1 : 0, StereoVsTranslateCount());
    return;
  }
}

// ---- Mode 34: VsRet(0x2C73E) stack -> FUNCTION STARTS -> dual ----
constexpr uint32_t kMode34SoftWaitMaxEs = 360;
constexpr uint32_t kMode34DiscoverEs = 90;
constexpr uint32_t kMode34CountEsNeed = 45;
constexpr uint32_t kMode34VsRetRva = 0x2C73E;

// Crash-probe: wrong-ABI hooks can hard-kill OUTSIDE SEH (stack imbalance on return).
// Write RVA before InstallDrawWalkAt; clear on dual/fallback; on next load recover → 30.
bool Mode34ProbePath(char* out, DWORD outLen) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&Mode34ProbePath), &self))
    return false;
  if (!GetModuleFileNameA(self, out, outLen))
    return false;
  char* slash = strrchr(out, '\\');
  if (!slash)
    slash = strrchr(out, '/');
  if (!slash)
    return false;
  slash[1] = 0;
  strcat_s(out, outLen, "gtaiv_dxvk_vr.mode34probe");
  return true;
}

void Mode34WriteProbe(uint32_t rva) {
  char path[MAX_PATH]{};
  if (!Mode34ProbePath(path, MAX_PATH))
    return;
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return;
  fprintf(f, "0x%X\n", rva);
  fclose(f);
  Log("Mode34: probe armed exeRva=0x%X (if process dies before clear → next load falls to 30)",
      rva);
}

void Mode34ClearProbe(const char* why) {
  char path[MAX_PATH]{};
  if (!Mode34ProbePath(path, MAX_PATH))
    return;
  if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    return;
  DeleteFileA(path);
  if (why)
    Log("Mode34: probe cleared - %s", why);
}

bool Mode34ConsumeCrashProbe() {
  char path[MAX_PATH]{};
  if (!Mode34ProbePath(path, MAX_PATH))
    return false;
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return false;
  char buf[32]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  DeleteFileA(path);
  unsigned rva = 0;
  if (n > 0)
    sscanf_s(buf, "0x%X", &rva);
  Log("Mode34: CRASH RECOVERY - leftover probe exeRva=0x%X (prior COUNT hard-kill) → "
      "forcing stereo=30",
      rva);
  WriteStereoModeFile(30);
  return true;
}

bool Mode34ForbiddenRva(uint32_t rva) {
  // 0x1BF010: COUNT hook hard-killed (SSE [ebp+8] stackarg misclassified as thiscall-frame).
  // 0x52E7C0: PE confirms [esp+34] stackarg — never hook.
  // 0x4DDAD0: COUNT ok + DUAL armed, but vsPatch=0 / vsCallsR=0 forever (same-cam dual,
  // no parallax). Do not re-arm — StereoDiff gate also failed to catch canvas noise.
  return Mode33ForbiddenRva(rva) || rva == kMode34VsRetRva || rva == 0x1BF010 ||
         rva == 0x52E7C0 || rva == 0x4DDAD0;
}

bool Mode34IsCcPaddedThiscall0(uintptr_t addr, const char** tagOut) {
  const char* tag = "?";
  if (!Mode33IsCcPaddedThiscall0(addr, &tag)) {
    if (tagOut)
      *tagOut = tag;
    return false;
  }
  // Only classic push+mov / 83EC thiscall0 shapes. thiscall-frame is too broad
  // (0x1BF010 SSE stackarg hard-killed outside SEH).
  const bool okTag = tag && (std::strcmp(tag, "thiscall-56") == 0 ||
                             std::strcmp(tag, "thiscall-5657") == 0 ||
                             std::strcmp(tag, "thiscall-535657") == 0 ||
                             std::strcmp(tag, "thiscall-53555657") == 0 ||
                             std::strcmp(tag, "thiscall-83EC") == 0);
  if (!okTag) {
    if (tagOut)
      *tagOut = (tag && std::strstr(tag, "frame")) ? "thiscall-frame-reject" : tag;
    return false;
  }
  if (tagOut)
    *tagOut = tag;
  return true;
}

// Read-only PE scan: static E8 → VsRet(0x2C73E). Logs caller sites + resolved starts
// with Mode34 ABI gates. No hooks. Feeds next same-frame attempt without Mode34 dual.
void LogStaticVsRetCallersOnce() {
  static std::atomic<bool> s_done{false};
  if (s_done.exchange(true))
    return;

  HMODULE mod = GetModuleHandleA(nullptr);
  if (!mod)
    return;
  const auto* baseBytes = reinterpret_cast<const uint8_t*>(mod);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(baseBytes);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return;
  const auto* nt =
      reinterpret_cast<const IMAGE_NT_HEADERS*>(baseBytes + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return;
  const uintptr_t base = reinterpret_cast<uintptr_t>(mod);
  const uintptr_t imgEnd = base + nt->OptionalHeader.SizeOfImage;
  const uintptr_t target = base + kMode34VsRetRva;

  uint32_t callSites = 0;
  uint32_t safeStarts = 0;
  uint32_t logged = 0;
  // Walk executable sections only (skip IAT/data false positives).
  const auto* sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned si = 0; si < nt->FileHeader.NumberOfSections; ++si) {
    if (!(sec[si].Characteristics & IMAGE_SCN_MEM_EXECUTE))
      continue;
    const uintptr_t s0 = base + sec[si].VirtualAddress;
    uintptr_t s1 = s0 + sec[si].Misc.VirtualSize;
    if (s1 > imgEnd)
      s1 = imgEnd;
    if (s1 < s0 + 5)
      continue;
    for (uintptr_t p = s0; p + 5 <= s1; ++p) {
      if (*reinterpret_cast<const uint8_t*>(p) != 0xE8)
        continue;
      const int32_t rel = *reinterpret_cast<const int32_t*>(p + 1);
      const uintptr_t dest = p + 5 + static_cast<intptr_t>(rel);
      if (dest != target)
        continue;
      ++callSites;
      const uint32_t callRva = static_cast<uint32_t>(p - base);
      uintptr_t fn = FindThiscallNear(p, 0x400);
      if (!fn)
        fn = FindFnStartNear(p);
      const uint32_t fnRva = fn ? static_cast<uint32_t>(fn - base) : 0;
      const char* tag = "no-start";
      bool safe = false;
      if (fn) {
        safe = Mode34IsCcPaddedThiscall0(fn, &tag) && !Mode34ForbiddenRva(fnRva);
        if (safe)
          ++safeStarts;
      }
      if (logged < 24) {
        Log("VsRetStatic: E8@0x%X -> 0x2C73E fnRva=0x%X abi=%s safe=%d%s", callRva, fnRva,
            tag, safe ? 1 : 0, Mode34ForbiddenRva(fnRva) ? " FORBIDDEN" : "");
        ++logged;
      }
    }
  }
  Log("VsRetStatic: done callSites=%u safeCcPadStarts=%u (read-only; dual still off; "
      "Mode30 playable) — next same-frame: prefer safe=1 RVAs",
      callSites, safeStarts);
}

void Mode34NoteFrameRva(uint32_t rva) {
  if (Mode34ForbiddenRva(rva))
    return;
  ++g_mode34SampleHits;
  for (int i = 0; i < g_mode34FrameN; ++i) {
    if (g_mode34Frame[i].rva == rva) {
      ++g_mode34Frame[i].count;
      return;
    }
  }
  if (g_mode34FrameN >= 96)
    return;
  g_mode34Frame[g_mode34FrameN++] = {rva, 1};
}

void Mode34AggRare(uint32_t rva, uint32_t hits) {
  for (int i = 0; i < g_mode34AggN; ++i) {
    if (g_mode34Agg[i].rva == rva) {
      g_mode34Agg[i].totalHits += hits;
      ++g_mode34Agg[i].rareFrames;
      return;
    }
  }
  if (g_mode34AggN >= 64)
    return;
  g_mode34Agg[g_mode34AggN++] = {rva, 1, hits};
}

void Mode34FallbackPairHold(const char* why) {
  g_mode34Phase.store(static_cast<int>(Mode34Phase::FallbackPairHold));
  g_vsPatchOn.store(false);
  g_drawWalkDead.store(true);
  SetStereoEye(StereoEye::Left);
  g_pairAwaitingR = false;
  if (g_drawWalkAddr) {
    MH_DisableHook(g_drawWalkAddr);
    MH_RemoveHook(g_drawWalkAddr);
    g_origDrawWalk = nullptr;
    g_drawWalkAddr = nullptr;
  }
  Mode34ClearProbe("fallback");
  WriteStereoModeFile(30);
  Log("Mode34: FALLBACK pair-hold - wrote stereo=30 - %s (kill -> 30 or 26)", why);
}

bool RunMode34DualGuarded(void* self, void* edx) {
  return RunMode32DualGuarded(self, edx);
}

bool Mode34StartNextCount() {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  while (g_mode34TryIdx < g_mode34TryN) {
    const uintptr_t start = g_mode34TryList[g_mode34TryIdx++];
    const uint32_t rva = static_cast<uint32_t>(start - base);
    const char* tag = "?";
    if (Mode34ForbiddenRva(rva) || !Mode34IsCcPaddedThiscall0(start, &tag)) {
      Log("Mode34: skip try exeRva=0x%X abi=%s (need CC-pad thiscall0 push/83EC)", rva, tag);
      continue;
    }
    Mode34WriteProbe(rva);
    if (!InstallDrawWalkAt(start)) {
      Mode34ClearProbe("MH fail");
      continue;
    }
    g_mode34ActiveRva = rva;
    g_mode34CountEs.store(0);
    g_mode34EntrySum = 0;
    g_drawWalkEntries.store(0);
    g_drawWalkDead.store(false);
    g_mode34Phase.store(static_cast<int>(Mode34Phase::Count));
    const auto* b = reinterpret_cast<const uint8_t*>(start);
    Log("Mode34: COUNT exeRva=0x%X abi=%s bytes=%02X %02X %02X %02X %02X %02X "
        "(need avg in [0.8,4] over %u EndScenes)",
        rva, tag, b[0], b[1], b[2], b[3], b[4], b[5], kMode34CountEsNeed);
    return true;
  }
  Mode34FallbackPairHold("no safe CC-pad thiscall0 VsRet-caller after discover");
  return false;
}

void Mode34BuildTryListFromDiscover() {
  g_mode34TryN = 0;
  g_mode34TryIdx = 0;
  int order[64];
  int n = 0;
  for (int i = 0; i < g_mode34AggN; ++i) {
    if (g_mode34Agg[i].rareFrames < 2)
      continue;
    // Same gate as COUNT: avg hits/rareFrame in [0.8, 4].
    const float avg =
        static_cast<float>(g_mode34Agg[i].totalHits) / static_cast<float>(g_mode34Agg[i].rareFrames);
    if (avg < 0.8f || avg > 4.f)
      continue;
    order[n++] = i;
  }
  // Rank by closeness to 1.0 entries/frame (true walker), then rareFrames.
  for (int a = 0; a < n; ++a) {
    for (int b = a + 1; b < n; ++b) {
      const Mode34Agg& A = g_mode34Agg[order[a]];
      const Mode34Agg& B = g_mode34Agg[order[b]];
      const float avgA = static_cast<float>(A.totalHits) / static_cast<float>(A.rareFrames);
      const float avgB = static_cast<float>(B.totalHits) / static_cast<float>(B.rareFrames);
      const float dA = avgA > 1.f ? avgA - 1.f : 1.f - avgA;
      const float dB = avgB > 1.f ? avgB - 1.f : 1.f - avgB;
      if (dB < dA || (dB == dA && B.rareFrames > A.rareFrames)) {
        const int t = order[a];
        order[a] = order[b];
        order[b] = t;
      }
    }
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  Log("Mode34: discover done aggN=%d rareCands=%d sampleHits=%u vsRetHits=%u vsTid=%u",
      g_mode34AggN, n, g_mode34SampleHits, g_mode34VsRetHits, g_mode34VsTid);
  for (int i = 0; i < n && i < 16; ++i) {
    const Mode34Agg& a = g_mode34Agg[order[i]];
    const float avg = static_cast<float>(a.totalHits) / static_cast<float>(a.rareFrames);
    Log("Mode34: rare startRva=0x%X rareFrames=%u totalHits=%u avg=%.2f", a.rva, a.rareFrames,
        a.totalHits, avg);
  }

  auto tryAdd = [&](uintptr_t fn, uint32_t fromRva, const char* how) {
    if (!fn || g_mode34TryN >= 8)
      return;
    const uint32_t fnRva = static_cast<uint32_t>(fn - base);
    const char* tag = "?";
    if (Mode34ForbiddenRva(fnRva) || !Mode34IsCcPaddedThiscall0(fn, &tag)) {
      const auto* b = reinterpret_cast<const uint8_t*>(fn);
      Log("Mode34: %s fromRva=0x%X -> fnRva=0x%X REJECT abi=%s bytes=%02X %02X %02X %02X", how,
          fromRva, fnRva, tag, b[0], b[1], b[2], b[3]);
      return;
    }
    for (int j = 0; j < g_mode34TryN; ++j) {
      if (g_mode34TryList[j] == fn)
        return;
    }
    g_mode34TryList[g_mode34TryN++] = fn;
    const auto* b = reinterpret_cast<const uint8_t*>(fn);
    Log("Mode34: try[%d] %s fromRva=0x%X -> fnRva=0x%X abi=%s bytes=%02X %02X %02X %02X %02X %02X",
        g_mode34TryN - 1, how, fromRva, fnRva, tag, b[0], b[1], b[2], b[3], b[4], b[5]);
  };

  // Agg already stores resolved FUNCTION STARTS (not mid epilogues).
  for (int i = 0; i < n && g_mode34TryN < 8; ++i) {
    const uint32_t startRva = g_mode34Agg[order[i]].rva;
    tryAdd(base + startRva, startRva, "vsRetCallerStart");
  }

  if (g_mode34TryN == 0) {
    Mode34FallbackPairHold("VsRet-caller hist found no CC-pad thiscall0 start");
    return;
  }
  Mode34StartNextCount();
}

void Mode34FinishDiscoverFrame(uint32_t es) {
  const uint32_t vsCalls = g_mode34VsCallsFrame.exchange(0);
  const int slots = g_mode34FrameN;
  for (int i = 0; i < g_mode34FrameN; ++i) {
    const uint32_t c = g_mode34Frame[i].count;
    // ~1x/frame among many VsRet uploads: keep rare starts only.
    if (c >= 1 && c <= 8)
      Mode34AggRare(g_mode34Frame[i].rva, c);
  }
  g_mode34FrameN = 0;
  if (es <= 6 || (es % 30) == 0)
    Log("Mode34: wait/discover es#%u onVs=%u rareAgg=%d frameSlots=%d sampleHits=%u "
        "vsRetHits=%u tid=%u",
        es, vsCalls, g_mode34AggN, slots, g_mode34SampleHits, g_mode34VsRetHits, g_mode34VsTid);
}

void Mode34OnEndScene(IDirect3DDevice9* device) {
  if (!device)
    return;
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    return;
  D3DSURFACE_DESC desc{};
  bb->GetDesc(&desc);
  bb->Release();
  uint32_t rtW = desc.Width, rtH = desc.Height;
  ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
  if (!EnsureEyeRts(device, rtW, rtH))
    return;
  if (!IsCamMatrixOverrideEnabled())
    return;

  const int ph = g_mode34Phase.load();
  const uint32_t es = ++g_mode34Es;

  if (ph == static_cast<int>(Mode34Phase::SoftWaitSamples) ||
      ph == static_cast<int>(Mode34Phase::FallbackPairHold)) {
    TemporalCapturePairHold(device);
    if (ph == static_cast<int>(Mode34Phase::SoftWaitSamples))
      Mode34FinishDiscoverFrame(es);
    if (ph == static_cast<int>(Mode34Phase::SoftWaitSamples)) {
      if (g_mode34AggN > 0 && g_mode34SampleHits >= 8 && g_mode34VsRetHits >= 8 && es >= 45) {
        g_mode34Phase.store(static_cast<int>(Mode34Phase::Discover));
        Log("Mode34: DISCOVER armed (VsRet callers -> fn starts; rareAgg=%d sampleHits=%u "
            "vsRetHits=%u; %u more ES) — pair-hold continues",
            g_mode34AggN, g_mode34SampleHits, g_mode34VsRetHits, kMode34DiscoverEs);
      } else if (es >= kMode34SoftWaitMaxEs) {
        Mode34FallbackPairHold("no VsRet-caller starts after soft wait - keep Mode 30");
      } else if (es >= 45 && (es % 60) == 0) {
        Log("Mode34: still waiting for VsRet-caller starts es=%u rareAgg=%d sampleHits=%u "
            "vsRetHits=%u",
            es, g_mode34AggN, g_mode34SampleHits, g_mode34VsRetHits);
      }
    }
    return;
  }

  if (ph == static_cast<int>(Mode34Phase::Discover)) {
    TemporalCapturePairHold(device);
    Mode34FinishDiscoverFrame(es);
    static uint32_t s_discStart34 = 0;
    if (s_discStart34 == 0)
      s_discStart34 = es;
    if (es - s_discStart34 >= kMode34DiscoverEs) {
      s_discStart34 = 0;
      Mode34BuildTryListFromDiscover();
    }
    return;
  }

  if (ph == static_cast<int>(Mode34Phase::Count)) {
    TemporalCapturePairHold(device);
    const uint32_t entries = g_drawWalkEntries.exchange(0);
    if (entries == 0xFFFFFFFFu) {
      Log("Mode34: count cand exeRva=0x%X died - try next", g_mode34ActiveRva);
      Mode34ClearProbe("count died");
      if (!Mode34StartNextCount())
        return;
      return;
    }
    ++g_mode34CountEs;
    g_mode34EntrySum += entries;
    const uint32_t n = g_mode34CountEs.load();
    if (n <= 6 || (n % 15) == 0)
      Log("Mode34: count es#%u/%u entries=%u sum=%u fnRva=0x%X", n, kMode34CountEsNeed,
          entries, g_mode34EntrySum, g_mode34ActiveRva);
    if (n >= kMode34CountEsNeed) {
      const float avg = static_cast<float>(g_mode34EntrySum) / static_cast<float>(n);
      if (avg >= 0.8f && avg <= 4.f) {
        // 2026-07-24: 0x4DDAD0 COUNT passed (avg=2.0) then DUAL ran 40k+ frames with
        // vsPatch=0 / vsCallsR=0 — walk is not a VS-upload walker → no parallax.
        // Do NOT arm dual. Playable stays Mode 30 pair-hold.
        Mode34ClearProbe("COUNT ok but dual disabled");
        Log("Mode34: COUNT ok fnRva=0x%X avgEntries=%.2f — dual DISABLED (prior "
            "0x4DDAD0 vsPatch=0/vsCallsR=0; no safe VS arming yet)",
            g_mode34ActiveRva, avg);
        Mode34FallbackPairHold("COUNT ok but dual disabled — keep Mode 30 pair-hold");
      } else {
        Log("Mode34: count REJECT fnRva=0x%X avgEntries=%.2f (want [0.8,4]) - try next",
            g_mode34ActiveRva, avg);
        Mode34ClearProbe("count reject");
        if (!Mode34StartNextCount())
          return;
      }
    }
    return;
  }

  if (ph == static_cast<int>(Mode34Phase::Dual)) {
    // Safety net if an older ASI left Dual armed: bail when VS never patches.
    const uint32_t dualN = g_mode34DualN.load();
    if (dualN >= 5 && StereoVsTranslateCount() == 0) {
      Mode34FallbackPairHold("vsPatch=0 after dual trial (same cam — keep Mode 30)");
      return;
    }
    if (g_haveL && g_haveR && (dualN == 5 || dualN == 30 ||
                               (dualN > 0 && (dualN % 300) == 0))) {
      const long long diff = CompareEyeCanvases(device);
      if (diff >= 0 && diff < 200) {
        const uint32_t z = ++g_mode34ZeroDiff;
        if (z >= 3)
          Mode34FallbackPairHold("StereoDiff~0 (dual same cam - VS patch not reaching draws)");
      } else if (diff >= 200) {
        g_mode34ZeroDiff.store(0);
      }
    }
    if (es <= 8 || (es % 300) == 0)
      Log("Mode34: dual EndScene es#%u dualN=%u haveL=%d haveR=%d vsPatch=%u", es,
          dualN, g_haveL ? 1 : 0, g_haveR ? 1 : 0, StereoVsTranslateCount());
    return;
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
    g_haveL = g_haveR = false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    Log("StereoExecPhase: EXCEPTION stage=%ld code=0x%08X addr=%p (exeRva 0x%X)",
        g_execViewStage, g_execViewExcCode, reinterpret_cast<void*>(g_execViewExcAddr),
        static_cast<unsigned>(g_execViewExcAddr - base));
    if (GetStereoMode() == StereoMode::PhaseDualDeviceVs)
      Mode30FallbackPairHold("SEH in dual pass");
    else {
      g_execDualDead.store(true);
      Log("StereoExecPhase: PERMANENTLY disabled, native mono continues");
    }
    return;
  }
  const uint32_t n = ++g_execViewDualCount;
  if (n <= 6 || (n % 300) == 0)
    Log("StereoExecPhase: #%u haveL=%d haveR=%d skips=%u sep=%.0fcm vsPatch=%u "
        "vsCallsTotal=%u vsCallsR=%u mode30dev=%u",
        n, g_haveL ? 1 : 0, g_haveR ? 1 : 0, g_execViewSkips.load(),
        GetStereoSepMeters() * 100.f, StereoVsTranslateCount(), StereoVsTotalCalls(),
        StereoVsRightPassCalls(), g_mode30DevPatches.load());
  if (g_haveL && g_haveR && (n == 5 || n == 30 || (n % 300) == 0)) {
    // Device VS push is the only Mode30 parallax lever. If it never matches,
    // dual is Mode23-identical-camera (no fusion) — fall back to pair-hold.
    if (GetStereoMode() == StereoMode::PhaseDualDeviceVs && n >= 5 &&
        g_mode30DevPatches.load() == 0) {
      Mode30FallbackPairHold(
          "deviceVsPatches=0 (Get/SetVSConstF found no view regs — dual=same cam)");
      return;
    }
    const long long diff = CompareEyeCanvases(g_device);
    if (GetStereoMode() == StereoMode::PhaseDualDeviceVs && diff >= 0 && diff < 200) {
      const uint32_t z = ++g_mode30ZeroDiff;
      if (z >= 2) {
        Mode30FallbackPairHold("StereoDiff~0 after device VS push (draws ignore device regs)");
        return;
      }
    } else if (diff >= 200) {
      g_mode30ZeroDiff.store(0);
    }
  }
  if (n == 600 || n == 3000)
    StereoVsDumpRets();
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
  // Mode 30/35/36 soft-start / pair-hold: passthrough native exec; EndScene does temporal.
  if (mode == StereoMode::PhaseDualDeviceVs || mode == StereoMode::FovRecomputeSite ||
      mode == StereoMode::FovRecomputeTrueCanvas) {
    const int ph = g_mode30Phase.load();
    if (ph != static_cast<int>(Mode30Phase::Dual) || g_execDualDead.load()) {
      g_origExecA(self, edx);
      return;
    }
  }
  // Mode 23/24/30: per-phase double exec + cam / device-VS eye shift.
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

// Mode 30 soft-start / fallback: capture into hold RTs; promote BOTH submit RTs
// only when a complete L→R pair is ready (Halo AFR parity lesson — no half-pair).
void TemporalCapturePairHold(IDirect3DDevice9* device) {
  if (!device || !g_holdL || !g_holdR || !g_texL || !g_texR)
    return;
  LogCachedIpdOnce();
  const StereoEye eye = GetStereoEye();
  if (eye == StereoEye::Left) {
    if (CopyBbToEyeCanvas(device, g_holdL, vr::Eye_Left))
      g_pairAwaitingR = true;
    SetStereoEye(StereoEye::Right);
  } else {
    if (CopyBbToEyeCanvas(device, g_holdR, vr::Eye_Right) && g_pairAwaitingR) {
      PromoteHoldPair(device);
      g_pairAwaitingR = false;
    }
    SetStereoEye(StereoEye::Left);
  }
  RefreshLiveCamForStereoEye();
  const uint32_t n = ++g_temporalFrames;
  if (n <= 6 || (n % 120) == 0)
    Log("StereoPairHold: frame #%u captured=%s awaitingR=%d haveSubmit=%d/%d sep=%.0fcm", n,
        eye == StereoEye::Left ? "L" : "R", g_pairAwaitingR ? 1 : 0, g_haveL ? 1 : 0,
        g_haveR ? 1 : 0, GetStereoSepMeters() * 100.f);
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

  if (mode == StereoMode::ReplayRootProbe || mode == StereoMode::VsParentCountProbe) {
    // FAILED 2026-07-24 (Mode 29 log): FindFnStartNear(Cand37C01) → 0x37BD0
    // prologue 56 57 8B FA, then "Cand37C01 EXCEPTION" and crash after load.
    // Same wrong-ABI site Mode 27 already rejected. Do NOT install count hooks.
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_execDualDead.store(false);
    g_execViewDualCount.store(0);
    g_execViewSkips.store(0);
    g_vsPatchOn.store(false);
    g_ok = InstallRootProbeHooks(2);
    Log("StereoRender: mode %d DISABLED (VsParent FindFnStartNear wrong ABI @0x37BD0 "
        "crashed after load) → Mode26 temporal alias ok=%d — set stereo file to 26",
        static_cast<int>(mode), g_ok.load() ? 1 : 0);
    return g_ok.load();
  }

  if (mode == StereoMode::SameFrameWrapDual) {
    // Crash after load (2026-07-24): VS wrapper hook @0x2C6AC is unsafe — do NOT
    // install it. Behave exactly like Mode 26 temporal until a safer root exists.
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_execDualDead.store(false);
    g_execViewDualCount.store(0);
    g_execViewSkips.store(0);
    g_vsPatchOn.store(false);
    g_ok = InstallRootProbeHooks(2);
    Log("StereoRender: mode 28 DISABLED (wrap@0x%X crashed after load) → Mode26 temporal "
        "alias ok=%d — set stereo file to 26",
        kVsWrapRva, g_ok.load() ? 1 : 0);
    return g_ok.load();
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

  if (mode == StereoMode::RecordDualReplayShift) {
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_execDualDead.store(false);
    g_execViewDualCount.store(0);
    g_execViewSkips.store(0);
    g_vsPatchOn.store(false);
    g_ok = InstallRootProbeHooks(2);  // ExecRoot (passthrough) + BuildRootA (count only)
    Log("StereoRender: mode 26 = Mode14 CCam temporal (no VS stack; soft-start 90; "
        "scale/IPD HMD defaults; bars=canvas) ok=%d",
        g_ok.load() ? 1 : 0);
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 14 or 0");
    return g_ok.load();
  }

  if (mode == StereoMode::BuildDualViewShift) {
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_execDualDead.store(false);
    g_execViewDualCount.store(0);
    g_execViewSkips.store(0);
    g_vsPatchOn.store(false);
    g_ok = InstallRootProbeHooks(2);  // ExecRoot (passthrough) + BuildRootA
    Log("StereoRender: mode 25 BUILD dual + VIEW shift (BuildRootA x2, manager matrices "
        "@0x%X/0x%X + VS-const translate in walk 2; expect ~half FPS) ok=%d",
        kMgrCamPosRvaA, kMgrCamPosRvaB, g_ok.load() ? 1 : 0);
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

  if (mode == StereoMode::PhaseDualDeviceVs || mode == StereoMode::FovRecomputeSite ||
      mode == StereoMode::FovRecomputeTrueCanvas) {
    // Pair-hold only: same install surface as Mode 26 (no exec dual hooks).
    // Device-VS dual was probed this session (mode30dev=0) — do not re-arm.
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_mode30Phase.store(static_cast<int>(Mode30Phase::PairHold));
    g_execDualDead.store(true);
    g_mode30SoftEs.store(0);
    g_mode30DevPatches.store(0);
    g_mode30ZeroDiff.store(0);
    g_pairAwaitingR = false;
    g_pairPromoteCount.store(0);
    g_vsPatchOn.store(false);
    g_ok = InstallRootProbeHooks(2);
    if (mode == StereoMode::FovRecomputeTrueCanvas) {
      const bool fovOk = InstallFovRecomputeSiteHook();
      Log("StereoRender: mode 36 FOV-RECOMPUTE + TRUE-CANVAS + Mode30 pair-hold "
          "(CCam FOV after fovadd → canvas tangents; no CanvasZoom; no D3DTS_PROJECTION) "
          "ok=%d fovSite=%d",
          g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=35 (keep fovadd) or stereo=30 + delete "
          "gtaiv_dxvk_vr.fovadd");
    } else if (mode == StereoMode::FovRecomputeSite) {
      const bool fovOk = InstallFovRecomputeSiteHook();
      Log("StereoRender: mode 35 FOV-RECOMPUTE + Mode30 pair-hold "
          "(FusionFix CCam+0x60 site; fovadd=ADD deg; canvas zoom OFF) ok=%d fovSite=%d",
          g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 30 + delete "
          "gtaiv_dxvk_vr.fovadd");
    } else {
      LogStaticVsRetCallersOnce();
      Log("StereoRender: mode 30 PAIR-HOLD (CCam temporal + promote L+R together; "
          "device-VS dual probed dead mode30dev=0; no 0x2C6AC/0x37BD0; canvas zoom OFF) ok=%d",
          g_ok.load() ? 1 : 0);
      Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 26 or 0");
    }
    return g_ok.load();
  }

  if (mode == StereoMode::SameFrameReplayDual) {
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_mode31Phase.store(static_cast<int>(Mode31Phase::SoftPairHold));
    g_mode31Es.store(0);
    g_mode31CountEs.store(0);
    g_mode31DualN.store(0);
    g_mode31ZeroDiff.store(0);
    g_mode31VsCallsFrame.store(0);
    g_mode31VsTid = 0;
    g_mode31EntrySum = 0;
    g_mode31FrameN = 0;
    g_mode31AggN = 0;
    g_mode31TryN = 0;
    g_mode31TryIdx = 0;
    g_mode31ActiveRva = 0;
    g_pairAwaitingR = false;
    g_pairPromoteCount.store(0);
    g_vsPatchOn.store(false);
    g_drawWalkDead.store(false);
    g_drawWalkAddr = nullptr;
    g_origDrawWalk = nullptr;
    g_ok = InstallRootProbeHooks(2);
    Log("StereoRender: mode 31 SAME-FRAME REPLAY dual (soft=Mode30 pair-hold → discover "
        "VsRet stack hist → count-only thiscall ≥45 ES → dual+VS; never 0x2C6AC/0x37BD0) "
        "ok=%d",
        g_ok.load() ? 1 : 0);
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 30 or 26");
    return g_ok.load();
  }

  if (mode == StereoMode::SameFrameVsParentDual) {
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_mode32Phase.store(static_cast<int>(Mode32Phase::SoftPairHold));
    g_mode32Es.store(0);
    g_mode32CountEs.store(0);
    g_mode32DualN.store(0);
    g_mode32ZeroDiff.store(0);
    g_mode32VsCallsFrame.store(0);
    g_mode32VsTid = 0;
    g_mode32EntrySum = 0;
    g_mode32FrameN = 0;
    g_mode32AggN = 0;
    g_mode32TryN = 0;
    g_mode32TryIdx = 0;
    g_mode32ActiveRva = 0;
    g_pairAwaitingR = false;
    g_pairPromoteCount.store(0);
    g_vsPatchOn.store(false);
    g_drawWalkDead.store(false);
    g_drawWalkAddr = nullptr;
    g_origDrawWalk = nullptr;
    g_ok = InstallRootProbeHooks(2);
    Log("StereoRender: mode 32 SAME-FRAME VsParent dual (soft=Mode30 → discover at "
        "HookSetVSConstF depth → count thiscall ≥45 ES → dual+VS; never 0x2C6AC/0x37BD0; "
        "fail writes stereo=30) ok=%d",
        g_ok.load() ? 1 : 0);
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 30 or 26");
    return g_ok.load();
  }

  if (mode == StereoMode::SameFrameLateVsParentDual) {
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_mode33Phase.store(static_cast<int>(Mode33Phase::SoftWaitSamples));
    g_mode33Es.store(0);
    g_mode33CountEs.store(0);
    g_mode33DualN.store(0);
    g_mode33ZeroDiff.store(0);
    g_mode33VsCallsFrame.store(0);
    g_mode33VsTid = 0;
    g_mode33EntrySum = 0;
    g_mode33SampleHits = 0;
    g_mode33FrameN = 0;
    g_mode33AggN = 0;
    g_mode33TryN = 0;
    g_mode33TryIdx = 0;
    g_mode33ActiveRva = 0;
    g_pairAwaitingR = false;
    g_pairPromoteCount.store(0);
    g_vsPatchOn.store(false);
    g_drawWalkDead.store(false);
    g_drawWalkAddr = nullptr;
    g_origDrawWalk = nullptr;
    g_ok = InstallRootProbeHooks(2);
    Log("StereoRender: mode 33 SAME-FRAME late-VsParent dual (WAIT live samples → "
        "CC-pad thiscall0 count ≥45 ES → dual+VS; never 0x2C6AC/0x37BD0/0x32A40/0x372B0; "
        "fail writes stereo=30) ok=%d",
        g_ok.load() ? 1 : 0);
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 30 or 26");
    return g_ok.load();
  }

  if (mode == StereoMode::SameFrameVsRetCallerDual) {
    // Prior COUNT hard-kill leaves mode34probe — recover to playable Mode 30.
    if (Mode34ConsumeCrashProbe()) {
      ReloadStereoMode();
      SetStereoEye(StereoEye::Left);
      g_haveL = g_haveR = false;
      g_mode30Phase.store(static_cast<int>(Mode30Phase::PairHold));
      g_execDualDead.store(true);
      g_mode30SoftEs.store(0);
      g_mode30DevPatches.store(0);
      g_mode30ZeroDiff.store(0);
      g_pairAwaitingR = false;
      g_pairPromoteCount.store(0);
      g_vsPatchOn.store(false);
      g_ok = InstallRootProbeHooks(2);
      Log("StereoRender: mode 34 CRASH-RECOVERY → mode %d PAIR-HOLD ok=%d",
          static_cast<int>(GetStereoMode()), g_ok.load() ? 1 : 0);
      Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 26 or 0");
      return g_ok.load();
    }
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_mode34Phase.store(static_cast<int>(Mode34Phase::SoftWaitSamples));
    g_mode34Es.store(0);
    g_mode34CountEs.store(0);
    g_mode34DualN.store(0);
    g_mode34ZeroDiff.store(0);
    g_mode34VsCallsFrame.store(0);
    g_mode34VsTid = 0;
    g_mode34EntrySum = 0;
    g_mode34SampleHits = 0;
    g_mode34VsRetHits = 0;
    g_mode34FrameN = 0;
    g_mode34AggN = 0;
    g_mode34TryN = 0;
    g_mode34TryIdx = 0;
    g_mode34ActiveRva = 0;
    g_pairAwaitingR = false;
    g_pairPromoteCount.store(0);
    g_vsPatchOn.store(false);
    g_drawWalkDead.store(false);
    g_drawWalkAddr = nullptr;
    g_origDrawWalk = nullptr;
    g_ok = InstallRootProbeHooks(2);
    LogStaticVsRetCallersOnce();
    Log("StereoRender: mode 34 SAME-FRAME VsRet-caller probe (WAIT ret==0x2C73E stack -> "
        "fn starts; COUNT only; DUAL DISABLED — 0x4DDAD0 proved vsPatch=0; "
        "never 0x2C6AC/0x37BD0/0x1BF010/0x4DDAD0; fail/COUNT-ok writes stereo=30) ok=%d",
        g_ok.load() ? 1 : 0);
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 30 or 26");
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
    Log("StereoRender: kill-switch - set gtaiv_dxvk_vr.stereo to 26 or 0 if freeze");
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

  // Modes 27/29: count hooks removed — behave as Mode 26 temporal (below).
  if (mode == StereoMode::RecordDualReplayShift || mode == StereoMode::SameFrameWrapDual ||
      mode == StereoMode::ReplayRootProbe || mode == StereoMode::VsParentCountProbe) {
    // After soft-start: Mode 14's full CCam temporal eye flip (moves ALL geometry —
    // VS-only felt like a flat monitor because only identity-world draws shifted)
    // PLUS VS view-translate armed for the next RIGHT frame (baked constants).
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
      return;
    D3DSURFACE_DESC desc{};
    bb->GetDesc(&desc);
    bb->Release();
    uint32_t rtW = desc.Width, rtH = desc.Height;
    ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
    if (!EnsureEyeRts(device, rtW, rtH))
      return;

    static uint32_t s_es = 0;
    static uint32_t s_warm = 0;
    if (++s_es == 1)
      Log("StereoRecDual: EndScene threadId=%u (must equal VsRet tid)", GetCurrentThreadId());

    if (!IsCamMatrixOverrideEnabled() || g_execDualDead.load() ||
        g_execViewDualCount.load() < 30) {
      g_vsPatchOn.store(false);
      s_warm = 0;
      return;
    }
    if (s_warm < 90) {
      ++s_warm;
      g_vsPatchOn.store(false);
      if (s_warm == 1 || s_warm == 90)
        Log("StereoRecDual: warm %u/90 (no eye flip yet)", s_warm);
      return;
    }

    // Mode 14 temporal path ONLY. Do NOT also arm the VS view-translate: CCam
    // already applies the full eye offset; stacking VS on identity-world draws
    // doubled IPD on walls/floors while props stayed single → flat/"monitor" 3D
    // and weird scale (headset 2026-07-24). Desktop L/R jump + look-smear are
    // the remaining temporal artifacts (need same-frame next).
    TemporalCaptureThisFrame(device);
    g_vsPatchOn.store(false);

    if (s_es <= 8 || (s_es % 600) == 0)
      Log("StereoRecDual: es#%u nextEye=%s haveL=%d haveR=%d sep=%.0fcm scale=%.2f "
          "(CCam only, no VS stack)",
          s_es, GetStereoEye() == StereoEye::Right ? "R" : "L", g_haveL ? 1 : 0,
          g_haveR ? 1 : 0, GetStereoSepMeters() * 100.f, GetWorldScale());
    return;
  }

    // Mode 30 / 35 / 36: pair-hold temporal only (device-VS dual proven dead — mode30dev=0).
    if (mode == StereoMode::PhaseDualDeviceVs || mode == StereoMode::FovRecomputeSite ||
        mode == StereoMode::FovRecomputeTrueCanvas) {
      g_dualDoneThisFrame = false;
      g_skipExecA = g_skipExecC = g_skipExecD = 0;
      IDirect3DSurface9* bb = nullptr;
      if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return;
      D3DSURFACE_DESC desc{};
      bb->GetDesc(&desc);
      bb->Release();
      uint32_t rtW = desc.Width, rtH = desc.Height;
      ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
      if (!EnsureEyeRts(device, rtW, rtH))
        return;
      if (!IsCamMatrixOverrideEnabled())
        return;
      // Keep phase on PairHold even if an older build left SoftStart/Dual.
      g_mode30Phase.store(static_cast<int>(Mode30Phase::PairHold));
      g_execDualDead.store(true);
      TemporalCapturePairHold(device);
      return;
    }

    if (mode == StereoMode::SameFrameReplayDual) {
      Mode31OnEndScene(device);
      return;
    }

    if (mode == StereoMode::SameFrameVsParentDual) {
      Mode32OnEndScene(device);
      return;
    }

    if (mode == StereoMode::SameFrameLateVsParentDual) {
      Mode33OnEndScene(device);
      return;
    }

    if (mode == StereoMode::SameFrameVsRetCallerDual) {
      Mode34OnEndScene(device);
      return;
    }

    if (mode == StereoMode::SameFrameRootDual || mode == StereoMode::ExecViewDual ||
      IsExecViewPhaseMode(mode) || mode == StereoMode::BuildDualViewShift) {
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
      mode != StereoMode::ExecViewDual && !IsExecViewPhaseMode(mode) &&
      mode != StereoMode::BuildDualViewShift && mode != StereoMode::RecordDualReplayShift &&
      mode != StereoMode::SameFrameWrapDual && mode != StereoMode::ReplayRootProbe &&
      mode != StereoMode::VsParentCountProbe && mode != StereoMode::PhaseDualDeviceVs &&
      mode != StereoMode::SameFrameReplayDual && mode != StereoMode::SameFrameVsParentDual &&
      mode != StereoMode::SameFrameLateVsParentDual && mode != StereoMode::SameFrameVsRetCallerDual &&
      mode != StereoMode::FovRecomputeSite && mode != StereoMode::FovRecomputeTrueCanvas)
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

bool StereoMode31WantsDiscover() {
  if (GetStereoMode() != StereoMode::SameFrameReplayDual)
    return false;
  const int ph = g_mode31Phase.load();
  // Collect during Soft too — otherwise we miss uploads before DISCOVER arms.
  return ph == static_cast<int>(Mode31Phase::SoftPairHold) ||
         ph == static_cast<int>(Mode31Phase::Discover);
}

void StereoMode31OnVsConst(void* retAddr) {
  if (!StereoMode31WantsDiscover())
    return;
  g_mode31VsCallsFrame.fetch_add(1);
  const uint32_t tid = GetCurrentThreadId();
  if (!g_mode31VsTid)
    g_mode31VsTid = tid;
  // Still record stack on other threads (log tid mismatch once).
  static bool s_tidWarn = false;
  if (tid != g_mode31VsTid && !s_tidWarn) {
    s_tidWarn = true;
    Log("Mode31: SetVSConstF tid mismatch first=%u other=%u", g_mode31VsTid, tid);
  }

  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  auto notePtr = [&](uintptr_t v) {
    if (v < base + 0x1000 || v > base + 0xC00000)
      return;
    Mode31NoteFrameRva(static_cast<uint32_t>(v - base));
  };
  notePtr(reinterpret_cast<uintptr_t>(retAddr));

  void* stack[10]{};
  const USHORT n = CaptureStackBackTrace(1, 8, stack, nullptr);
  if (n > 0) {
    for (USHORT i = 0; i < n; ++i)
      notePtr(reinterpret_cast<uintptr_t>(stack[i]));
  }
  // Always also scan stack words (FPO often makes CaptureStackBackTrace empty).
  auto* sp = reinterpret_cast<uintptr_t*>(_AddressOfReturnAddress());
  for (int i = 1; i < 40; ++i)
    notePtr(sp[i]);

  static int s_sample = 0;
  if (s_sample < 8) {
    ++s_sample;
    Log("Mode31: VsSample #%d retRva=0x%X tid=%u frameSlots=%d", s_sample,
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(retAddr) - base), tid,
        g_mode31FrameN);
  }
}

bool StereoMode32WantsDiscover() {
  if (GetStereoMode() != StereoMode::SameFrameVsParentDual)
    return false;
  const int ph = g_mode32Phase.load();
  return ph == static_cast<int>(Mode32Phase::SoftPairHold) ||
         ph == static_cast<int>(Mode32Phase::Discover);
}

void StereoMode32CollectVsParents(void* retAddr, void* hookSpWords) {
  if (!StereoMode32WantsDiscover() || !hookSpWords)
    return;
  g_mode32VsCallsFrame.fetch_add(1);
  const uint32_t tid = GetCurrentThreadId();
  if (!g_mode32VsTid)
    g_mode32VsTid = tid;
  static bool s_tidWarn = false;
  if (tid != g_mode32VsTid && !s_tidWarn) {
    s_tidWarn = true;
    Log("Mode32: SetVSConstF tid mismatch first=%u other=%u", g_mode32VsTid, tid);
  }

  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  auto notePtr = [&](uintptr_t v) {
    if (v < base + 0x1000 || v > base + 0xC00000)
      return;
    Mode32NoteFrameRva(static_cast<uint32_t>(v - base));
  };
  // retAddr = VsRet (usually 0x2C73E) — filtered by Mode32NoteFrameRva.
  notePtr(reinterpret_cast<uintptr_t>(retAddr));

  // hookSpWords = _AddressOfReturnAddress() FROM HookSetVSConstF (Mode 26 depth).
  // Slot 0 = return into caller of HookSetVSConstF (= VsRet). Slots 1+ = VsParents.
  auto* sp = reinterpret_cast<uintptr_t*>(hookSpWords);
  for (int i = 0; i < 64; ++i)
    notePtr(sp[i]);

  static int s_sample = 0;
  if (s_sample < 12) {
    ++s_sample;
    int logged = 0;
    for (int i = 0; i < 64 && logged < 6; ++i) {
      const uintptr_t v = sp[i];
      if (v < base + 0x1000 || v > base + 0xC00000)
        continue;
      const unsigned rva = static_cast<unsigned>(v - base);
      if (Mode32ForbiddenRva(rva))
        continue;
      Log("Mode32: VsParent sample#%d slot=%d exeRva=0x%X tid=%u", s_sample, i, rva, tid);
      ++logged;
    }
    Log("Mode32: VsSample #%d retRva=0x%X tid=%u frameSlots=%d", s_sample,
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(retAddr) - base), tid,
        g_mode32FrameN);
  }
}

bool StereoMode33WantsDiscover() {
  if (GetStereoMode() != StereoMode::SameFrameLateVsParentDual)
    return false;
  const int ph = g_mode33Phase.load();
  return ph == static_cast<int>(Mode33Phase::SoftWaitSamples) ||
         ph == static_cast<int>(Mode33Phase::Discover);
}

void StereoMode33CollectVsParents(void* retAddr, void* hookSpWords) {
  if (!StereoMode33WantsDiscover() || !hookSpWords)
    return;
  g_mode33VsCallsFrame.fetch_add(1);
  const uint32_t tid = GetCurrentThreadId();
  if (!g_mode33VsTid)
    g_mode33VsTid = tid;
  static bool s_tidWarn = false;
  if (tid != g_mode33VsTid && !s_tidWarn) {
    s_tidWarn = true;
    Log("Mode33: SetVSConstF tid mismatch first=%u other=%u", g_mode33VsTid, tid);
  }

  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  auto notePtr = [&](uintptr_t v) {
    if (v < base + 0x1000 || v > base + 0xC00000)
      return;
    Mode33NoteFrameRva(static_cast<uint32_t>(v - base));
  };
  notePtr(reinterpret_cast<uintptr_t>(retAddr));

  auto* sp = reinterpret_cast<uintptr_t*>(hookSpWords);
  for (int i = 0; i < 64; ++i)
    notePtr(sp[i]);

  static int s_sample = 0;
  if (s_sample < 16) {
    ++s_sample;
    int logged = 0;
    for (int i = 0; i < 64 && logged < 8; ++i) {
      const uintptr_t v = sp[i];
      if (v < base + 0x1000 || v > base + 0xC00000)
        continue;
      const unsigned rva = static_cast<unsigned>(v - base);
      if (Mode33ForbiddenRva(rva))
        continue;
      Log("Mode33: VsParent sample#%d slot=%d exeRva=0x%X tid=%u", s_sample, i, rva, tid);
      ++logged;
    }
    Log("Mode33: VsSample #%d retRva=0x%X tid=%u frameSlots=%d sampleHits=%u", s_sample,
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(retAddr) - base), tid, g_mode33FrameN,
        g_mode33SampleHits);
  }
}


bool StereoMode34WantsDiscover() {
  if (GetStereoMode() != StereoMode::SameFrameVsRetCallerDual)
    return false;
  const int ph = g_mode34Phase.load();
  return ph == static_cast<int>(Mode34Phase::SoftWaitSamples) ||
         ph == static_cast<int>(Mode34Phase::Discover);
}

void StereoMode34CollectVsRetCallers(void* retAddr, void* hookSpWords) {
  if (!StereoMode34WantsDiscover() || !hookSpWords || !retAddr)
    return;
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uint32_t retRva =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(retAddr) - base);
  // KEY vs Mode 33: only harvest when the VS upload returns into VsRet itself.
  if (retRva != kMode34VsRetRva)
    return;
  ++g_mode34VsRetHits;
  g_mode34VsCallsFrame.fetch_add(1);
  const uint32_t tid = GetCurrentThreadId();
  if (!g_mode34VsTid)
    g_mode34VsTid = tid;

  auto* sp = reinterpret_cast<uintptr_t*>(hookSpWords);
  // Resolve each stack word to a FUNCTION START (not mid epilogue).
  // Only keep CC-pad zero-arg thiscall shapes (filter early — no frame/stackarg noise).
  for (int i = 0; i < 64; ++i) {
    const uintptr_t v = sp[i];
    if (v < base + 0x1000 || v > base + 0xC00000)
      continue;
    // Prefer near thiscall start; fall back to FindFnStartNear.
    uintptr_t fn = FindThiscallNear(v, 0x300);
    if (!fn)
      fn = FindThiscallNear(v, 0x800);
    if (!fn)
      fn = FindFnStartNear(v);
    if (!fn)
      continue;
    const uint32_t fnRva = static_cast<uint32_t>(fn - base);
    if (Mode34ForbiddenRva(fnRva))
      continue;
    const char* tag = "?";
    if (!Mode34IsCcPaddedThiscall0(fn, &tag))
      continue;
    Mode34NoteFrameRva(fnRva);
  }

  static int s_sample = 0;
  if (s_sample < 16) {
    ++s_sample;
    int logged = 0;
    for (int i = 0; i < 64 && logged < 6; ++i) {
      const uintptr_t v = sp[i];
      if (v < base + 0x1000 || v > base + 0xC00000)
        continue;
      uintptr_t fn = FindThiscallNear(v, 0x800);
      if (!fn)
        fn = FindFnStartNear(v);
      if (!fn)
        continue;
      const unsigned fnRva = static_cast<unsigned>(fn - base);
      if (Mode34ForbiddenRva(fnRva))
        continue;
      Log("Mode34: VsRetCaller sample#%d slot=%d midRva=0x%X -> startRva=0x%X tid=%u",
          s_sample, i, static_cast<unsigned>(v - base), fnRva, tid);
      ++logged;
    }
    Log("Mode34: VsRet sample#%d retRva=0x%X tid=%u frameSlots=%d sampleHits=%u vsRetHits=%u",
        s_sample, retRva, tid, g_mode34FrameN, g_mode34SampleHits, g_mode34VsRetHits);
  }
}

}  // namespace asi
