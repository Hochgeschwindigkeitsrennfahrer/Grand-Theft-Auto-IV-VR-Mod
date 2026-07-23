#include "stereo_proj.h"
#include "cam_matrix.h"
#include "log.h"
#include "stereo_config.h"
#include "stereo_eye.h"
#include "vr_display.h"

#include "../../thirdparty/minhook/include/MinHook.h"

#include <d3d9.h>

#include <atomic>
#include <cmath>
#include <cstring>

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

// Cover-FOV widen + TextureBounds felt like a broken in-game FOV slider (User 2026-07-24).
// Mode 7 at ~90 FPS without that stack was the good baseline — keep widen OFF until
// we can match Rage projection properly (L4D2 sets engine FOV; we were patching D3D wrong).
bool ModeWantsCoverFov() {
  return false;
}

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
  if (state == D3DTS_PROJECTION && matrix && ModeWantsCoverFov() && IsCamMatrixOverrideEnabled()) {
    D3DMATRIX m = *matrix;
    if (WidenToCoverFov(&m.m[0][0]))
      return g_origSetTransform(self, state, &m);
  }
  return g_origSetTransform(self, state, matrix);
}

HRESULT STDMETHODCALLTYPE HookBeginScene(IDirect3DDevice9* self) {
  const HRESULT hr = g_origBeginScene(self);
  if (SUCCEEDED(hr) && ModeWantsCoverFov())
    StereoProjApplyForCurrentEye(self);
  return hr;
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
  g_hooksOk = true;
  Log("StereoProj: SetTransform+BeginScene OK (L4D2 cover FOV)");
}

}  // namespace asi
