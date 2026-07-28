#include "stereo_proj.h"
#include "cam_matrix.h"
#include "log.h"
#include "stereo_config.h"
#include "stereo_eye.h"
#include "stereo_render.h"
#include "vr_display.h"

#include "../../thirdparty/minhook/include/MinHook.h"

#include <d3d9.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include <intrin.h>  // _ReturnAddress: who uploads VS constants (real draw path)

namespace asi {
namespace {

using SetTransform_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE,
                                                   const D3DMATRIX*);
using BeginScene_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);

SetTransform_t g_origSetTransform = nullptr;
BeginScene_t g_origBeginScene = nullptr;
std::atomic<bool> g_hooksOk{false};
std::atomic<bool> g_logged{false};
std::atomic<uint32_t> g_patchCount{0};

// Legacy VS-constant-scan stack (BeginScene scans 0..60 every frame) — cost ~45 FPS
// and felt like a broken FOV slider. Keep OFF permanently.
bool ModeWantsCoverFov() {
  return false;
}

// Mode 15: widen ONLY the projection the game pushes via SetTransform. Zero per-frame
// cost. Mode 14's canvas reads the widened tangents back (GetTransform) and adapts.
// Outcome tells us if Rage consumes D3DTS_PROJECTION: border shrinks with correct
// proportions = yes; image vertically stretched = no (then mode 15 is dead, use 14).
bool ModeWantsProjWiden() {
  return GetStereoMode() == StereoMode::CanvasWide;
}

std::atomic<uint32_t> g_widenCount{0};

bool LooksLikeProjectionScales(const float* m) {
  return m && m[0] > 0.1f && m[0] < 8.f && m[5] > 0.1f && m[5] < 8.f;
}

bool LooksLikeProjectionRowMajor(const float* m) {
  if (!LooksLikeProjectionScales(m))
    return false;
  if (std::fabs(m[15]) > 0.08f)
    return false;
  if (std::fabs(m[11]) < 0.85f || std::fabs(m[11]) > 1.15f)
    return false;
  return true;
}

bool LooksLikeViewRowMajor(const float* m) {
  if (!m)
    return false;
  if (std::fabs(m[15] - 1.f) > 0.08f)
    return false;
  if (std::fabs(m[3]) > 0.08f || std::fabs(m[7]) > 0.08f || std::fabs(m[11]) > 0.08f)
    return false;
  const float l0 = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  const float l1 = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
  return l0 >= 0.85f && l0 <= 1.15f && l1 >= 0.85f && l1 <= 1.15f;
}

// L4D2VR: shared symmetric cover FOV. Per-eye asymmetry → TextureBounds on Submit.
bool WidenToCoverFov(float* m16) {
  if (!LooksLikeProjectionScales(m16))
    return false;
  float tanH = 0.f, tanV = 0.f;
  if (!GetCoverFovTangents(&tanH, &tanV))
    return false;
  m16[0] = 1.f / tanH;
  m16[5] = 1.f / tanV;
  m16[8] = 0.f;
  m16[9] = 0.f;
  return true;
}

HRESULT STDMETHODCALLTYPE HookSetTransform(IDirect3DDevice9* self, D3DTRANSFORMSTATETYPE state,
                                           const D3DMATRIX* matrix) {
  if (state == D3DTS_PROJECTION && matrix &&
      (ModeWantsCoverFov() || ModeWantsProjWiden()) && IsCamMatrixOverrideEnabled()) {
    D3DMATRIX m = *matrix;
    if (WidenToCoverFov(&m.m[0][0])) {
      const uint32_t n = ++g_widenCount;
      if (n <= 3 || (n % 600) == 0)
        Log("StereoProj: widen SetTransform #%u (mode 15)", n);
      return g_origSetTransform(self, state, &m);
    }
  }
  return g_origSetTransform(self, state, matrix);
}

HRESULT STDMETHODCALLTYPE HookBeginScene(IDirect3DDevice9* self) {
  const HRESULT hr = g_origBeginScene(self);
  if (SUCCEEDED(hr) && ModeWantsCoverFov())
    StereoProjApplyForCurrentEye(self);
  return hr;
}

// ---- Mode 24: view-translate in SetVertexShaderConstantF during the R pass ----
// Rage bakes the view/viewProj into shader constants at BUILD time; re-executing
// the draw lists replays those uploads through this D3D9 entry point. During the
// RIGHT pass we detect any 4x4 block that maps the build camera position onto the
// view axis (x'=y'=0 -> it is a view or viewProj matrix) and pre-multiply the
// world-space eye translation. Detection is convention-agnostic (row-vector and
// column-vector layouts).
using SetVSConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
SetVSConstF_t g_origSetVSConstF = nullptr;
std::atomic<uint32_t> g_vsTransCount{0};
std::atomic<uint32_t> g_vsRightCalls{0};
std::atomic<uint32_t> g_vsCallsTotal{0};  // ALL uploads (hook aliveness / flow diagnosis)

struct VsSeenReg {
  UINT reg;
  UINT cnt;
  int interp;  // 0 = seen only, 1 = row-vector match, 2 = column-vector match
};
VsSeenReg g_vsSeen[24]{};
int g_vsSeenN = 0;

void NoteVsReg(UINT reg, UINT cnt, int interp) {
  for (int i = 0; i < g_vsSeenN; ++i) {
    if (g_vsSeen[i].reg == reg && g_vsSeen[i].interp == interp)
      return;
  }
  if (g_vsSeenN >= 24)
    return;
  g_vsSeen[g_vsSeenN++] = {reg, cnt, interp};
  Log("VsPatch: %s reg=%u cnt=%u",
      interp == 1   ? "MATCH rowvec"
      : interp == 2 ? "MATCH colvec"
                    : "seen",
      reg, cnt);
}

// The re-executed phase objects issue ZERO uploads (vsCallsR=0, 2026-07-24) —
// the REAL draw path lives elsewhere. Log unique (returnAddress, threadId) of
// every uploader so the next dual attempt wraps the actual submission code.
struct VsRet {
  void* ret;
  uint32_t tid;
  uint32_t count;
};
VsRet g_vsRets[40]{};
int g_vsRetN = 0;

void NoteVsRet(void* ret) {
  const uint32_t tid = GetCurrentThreadId();
  for (int i = 0; i < g_vsRetN; ++i) {
    if (g_vsRets[i].ret == ret && g_vsRets[i].tid == tid) {
      ++g_vsRets[i].count;
      return;
    }
  }
  if (g_vsRetN >= 40)
    return;
  g_vsRets[g_vsRetN++] = {ret, tid, 1};
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  Log("VsRet: NEW ret=%p exeRva=0x%X tid=%u", ret,
      static_cast<unsigned>(reinterpret_cast<uintptr_t>(ret) - base), tid);

  // Mode 27 prep: x86 FPO often makes CaptureStackBackTrace too short. Scan the
  // stack for other return addresses inside GTAIV.exe (skip the wrapper itself).
  static int s_parentLogged = 0;
  if (s_parentLogged < 12) {
    auto* sp = reinterpret_cast<uintptr_t*>(_AddressOfReturnAddress());
    for (int i = 1; i < 48 && s_parentLogged < 12; ++i) {
      const uintptr_t v = sp[i];
      if (v < base + 0x1000 || v > base + 0xB00000)
        continue;
      const unsigned rva = static_cast<unsigned>(v - base);
      if (rva == static_cast<unsigned>(reinterpret_cast<uintptr_t>(ret) - base))
        continue;
      Log("VsParent: exeRva=0x%X tid=%u slot=%d (Mode 27 candidate)", rva, tid, i);
      ++s_parentLogged;
    }
  }
}

HRESULT STDMETHODCALLTYPE HookSetVSConstF(IDirect3DDevice9* self, UINT startReg,
                                          const float* data, UINT cnt) {
  ++g_vsCallsTotal;
  const StereoMode m = GetStereoMode();
  if (m == StereoMode::ExecViewConstDual || m == StereoMode::BuildDualViewShift ||
      m == StereoMode::RecordDualReplayShift || m == StereoMode::SameFrameWrapDual ||
      m == StereoMode::SameFrameVsParentDual || m == StereoMode::SameFrameLateVsParentDual ||
      m == StereoMode::SameFrameVsRetCallerDual)
    NoteVsRet(_ReturnAddress());
  if (StereoMode31WantsDiscover())
    StereoMode31OnVsConst(_ReturnAddress());
  // Mode 32/33/34: scan at THIS frame depth (Mode 26 NoteVsRet style). Passing SP into a
  // helper is OK; do NOT call _AddressOfReturnAddress from a deeper callee.
  if (StereoMode32WantsDiscover())
    StereoMode32CollectVsParents(_ReturnAddress(), _AddressOfReturnAddress());
  if (StereoMode33WantsDiscover())
    StereoMode33CollectVsParents(_ReturnAddress(), _AddressOfReturnAddress());
  if (StereoMode34WantsDiscover())
    StereoMode34CollectVsRetCallers(_ReturnAddress(), _AddressOfReturnAddress());
  if (StereoMode41WantsTrace())
    StereoMode41CollectVsRetChain(_ReturnAddress(), _AddressOfReturnAddress());
  if (StereoMode198WantsSample())
    StereoMode198CollectVsRet(_ReturnAddress(), _AddressOfReturnAddress());
  float cam[3], d[3];
  if (!data || cnt < 4 || cnt > 256 || !StereoVsGetPatchParams(cam, d))
    return g_origSetVSConstF(self, startReg, data, cnt);

  ++g_vsRightCalls;
  float buf[1024];
  std::memcpy(buf, data, cnt * 16u);
  const float cx = cam[0], cy = cam[1], cz = cam[2];
  const float dx = d[0], dy = d[1], dz = d[2];
  int patched = 0;

  // ONLY patch blocks that mathematically map the build cam onto the view
  // axis (identity-world V / VP). Speculative "every c4/c8 in a 16-reg upload"
  // patches (2026-07-24) corrupted non-Rage layouts and froze the GPU shortly
  // after the first MATCH — keep them out until we have a safer object path.
  for (UINT b = 0; b + 4 <= cnt; b += 4) {
    float* f = buf + b * 4;

    // Row-vector layout (p' = p*M): rows contiguous in memory.
    const float xa = cx * f[0] + cy * f[4] + cz * f[8] + f[12];
    const float ya = cx * f[1] + cy * f[5] + cz * f[9] + f[13];
    const float za = cx * f[2] + cy * f[6] + cz * f[10] + f[14];
    const float wa = cx * f[3] + cy * f[7] + cz * f[11] + f[15];
    const float gxa = std::fabs(cx * f[0]) + std::fabs(cy * f[4]) + std::fabs(cz * f[8]) +
                      std::fabs(f[12]);
    const float gya = std::fabs(cx * f[1]) + std::fabs(cy * f[5]) + std::fabs(cz * f[9]) +
                      std::fabs(f[13]);
    // Column-vector layout (p' = M*p): memory rows are matrix rows.
    const float xb = cx * f[0] + cy * f[1] + cz * f[2] + f[3];
    const float yb = cx * f[4] + cy * f[5] + cz * f[6] + f[7];
    const float zb = cx * f[8] + cy * f[9] + cz * f[10] + f[11];
    const float wb = cx * f[12] + cy * f[13] + cz * f[14] + f[15];
    const float gxb = std::fabs(cx * f[0]) + std::fabs(cy * f[1]) + std::fabs(cz * f[2]) +
                      std::fabs(f[3]);
    const float gyb = std::fabs(cx * f[4]) + std::fabs(cy * f[5]) + std::fabs(cz * f[6]) +
                      std::fabs(f[7]);

    // "Maps cam pos to the axis" with float-cancellation tolerance; reject the
    // zero matrix by requiring a nonzero z' (viewProj) or w' (affine view).
    const bool matchA = std::fabs(xa) < 2e-3f * gxa + 1e-3f &&
                        std::fabs(ya) < 2e-3f * gya + 1e-3f &&
                        (std::fabs(za) > 1e-3f || std::fabs(wa) > 0.5f) && gxa > 1e-3f;
    const bool matchB = std::fabs(xb) < 2e-3f * gxb + 1e-3f &&
                        std::fabs(yb) < 2e-3f * gyb + 1e-3f &&
                        (std::fabs(zb) > 1e-3f || std::fabs(wb) > 0.5f) && gxb > 1e-3f;

    if (matchA) {
      // V' = T(-delta) * V  (camera moved +delta => world moved -delta)
      f[12] -= dx * f[0] + dy * f[4] + dz * f[8];
      f[13] -= dx * f[1] + dy * f[5] + dz * f[9];
      f[14] -= dx * f[2] + dy * f[6] + dz * f[10];
      f[15] -= dx * f[3] + dy * f[7] + dz * f[11];
      ++patched;
      NoteVsReg(startReg + b, cnt, 1);
    } else if (matchB) {
      // V' = V * T_col(-delta)
      f[3] -= dx * f[0] + dy * f[1] + dz * f[2];
      f[7] -= dx * f[4] + dy * f[5] + dz * f[6];
      f[11] -= dx * f[8] + dy * f[9] + dz * f[10];
      f[15] -= dx * f[12] + dy * f[13] + dz * f[14];
      ++patched;
      NoteVsReg(startReg + b, cnt, 2);
    }
  }

  if (!patched) {
    // Layout intel for the log even when nothing matches (first uploads only).
    if (g_vsRightCalls.load() <= 40)
      NoteVsReg(startReg, cnt, 0);
    return g_origSetVSConstF(self, startReg, data, cnt);
  }
  g_vsTransCount.fetch_add(static_cast<uint32_t>(patched));
  return g_origSetVSConstF(self, startReg, buf, cnt);
}

}  // namespace

bool StereoProjShouldInject() {
  return ModeWantsCoverFov() && IsCamMatrixOverrideEnabled();
}

void StereoProjApplyForCurrentEye(IDirect3DDevice9* device) {
  if (!device || !ModeWantsCoverFov() || !IsCamMatrixOverrideEnabled())
    return;

  PushLiveCamToD3D(device);

  float view[16];
  const bool haveView = BuildLiveViewMatrix16(view);
  const bool right = GetStereoEye() == StereoEye::Right;

  D3DMATRIX proj{};
  if (SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &proj))) {
    float m[16];
    std::memcpy(m, &proj.m[0][0], sizeof(m));
    if (WidenToCoverFov(m))
      device->SetTransform(D3DTS_PROJECTION, reinterpret_cast<D3DMATRIX*>(m));
  }

  int patched = 0;
  if (haveView) {
    for (UINT base = 0; base <= 60; base += 4) {
      float block[16]{};
      if (FAILED(device->GetVertexShaderConstantF(base, block, 4)))
        continue;
      if (!LooksLikeViewRowMajor(block))
        continue;
      if (FAILED(device->SetVertexShaderConstantF(base, view, 4)))
        continue;
      ++patched;
    }
  }

  for (UINT base = 0; base <= 60; base += 4) {
    float block[16]{};
    if (FAILED(device->GetVertexShaderConstantF(base, block, 4)))
      continue;
    if (!LooksLikeProjectionRowMajor(block))
      continue;
    if (!WidenToCoverFov(block))
      continue;
    if (SUCCEEDED(device->SetVertexShaderConstantF(base, block, 4)))
      ++patched;
  }

  const uint32_t n = ++g_patchCount;
  if (!g_logged.exchange(true) || n <= 6 || (n % 300) == 0)
    Log("StereoProj: coverFOV eye=%s vsPatches=%d #%u", right ? "R" : "L", patched, n);
}

unsigned StereoVsTranslateCount() {
  return g_vsTransCount.load();
}

unsigned StereoVsTotalCalls() {
  return g_vsCallsTotal.load();
}

unsigned StereoVsRightPassCalls() {
  return g_vsRightCalls.load();
}

void StereoVsDumpRets() {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  for (int i = 0; i < g_vsRetN; ++i)
    Log("VsRet: dump exeRva=0x%X tid=%u count=%u",
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_vsRets[i].ret) - base),
        g_vsRets[i].tid, g_vsRets[i].count);
}

void InstallStereoProjHooks(IDirect3DDevice9* device) {
  if (!device || g_hooksOk.load())
    return;
  void** vt = *reinterpret_cast<void***>(device);
  if (MH_CreateHook(vt[44], reinterpret_cast<void*>(&HookSetTransform),
                    reinterpret_cast<void**>(&g_origSetTransform)) != MH_OK ||
      MH_EnableHook(vt[44]) != MH_OK) {
    Log("StereoProj: SetTransform hook FAIL");
    return;
  }
  if (MH_CreateHook(vt[41], reinterpret_cast<void*>(&HookBeginScene),
                    reinterpret_cast<void**>(&g_origBeginScene)) != MH_OK ||
      MH_EnableHook(vt[41]) != MH_OK) {
    Log("StereoProj: BeginScene hook FAIL");
    return;
  }
  // vt[94] = SetVertexShaderConstantF (mode 24 right-pass view translate; the
  // gate StereoVsGetPatchParams is false everywhere else -> zero overhead).
  if (MH_CreateHook(vt[94], reinterpret_cast<void*>(&HookSetVSConstF),
                    reinterpret_cast<void**>(&g_origSetVSConstF)) != MH_OK ||
      MH_EnableHook(vt[94]) != MH_OK) {
    Log("StereoProj: SetVertexShaderConstantF hook FAIL (mode 24 unavailable)");
  }
  g_hooksOk = true;
  Log("StereoProj: SetTransform+BeginScene+SetVSConstF OK");
}

}  // namespace asi
