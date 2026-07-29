#include "stereo_calib.h"

#include "cam_matrix.h"
#include "log.h"
#include "stereo_config.h"
#include "stereo_eye.h"
#include "vr_display.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>

#include <openvr.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

// Mode 230 — read-only projection calibration. See docs/STEREO_TRUETICK_REVIEW.md §4.1.
// 203-seam retry: measures what Mode 203 (@0x4D8BF0) actually renders.

namespace asi {
namespace {

// ~1.3 s at 90 fps. The projection only changes on zoom/cutscene, so this is plenty.
constexpr unsigned kSampleEveryPairs = 120;
// Probe distance in metres. Any value works; 2 m keeps us clear of the near plane and
// makes the viewProj discriminator (w == z_view) unambiguous against a pure view
// matrix (w == 1).
constexpr float kProbeDistM = 2.f;
constexpr float kPi = 3.14159265f;
constexpr unsigned kLumaDim = 8;

std::atomic<bool> g_arm{false};
std::atomic<unsigned> g_pairs{0};
std::atomic<unsigned> g_samples{0};
std::atomic<unsigned> g_uploadsSeen{0};
std::atomic<unsigned> g_camBasisFails{0};
std::atomic<unsigned> g_axisMatches{0};
unsigned g_missStreak = 0;
bool g_eyeDone[2] = {false, false};
bool g_loggedStatic = false;
bool g_loggedRawBlock = false;
bool g_loggedMiss = false;

IDirect3DSurface9* g_lumaRt = nullptr;
IDirect3DSurface9* g_lumaSys = nullptr;

bool IsProbeMode() {
  return GetStereoMode() == StereoMode::TrueStereoCalibProbe;
}

// Measure on 230 (log only) and 231 (log + publish gameTan).
bool IsMeasureMode() {
  return IsProbeMode() || IsTrueStereoExact(GetStereoMode());
}

std::atomic<bool> g_haveMeasurement{false};

bool GetAsiDirLocal(char* out, DWORD outLen) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(&GetAsiDirLocal), &self))
    return false;
  if (!GetModuleFileNameA(self, out, outLen))
    return false;
  char* slash = strrchr(out, '\\');
  if (!slash)
    slash = strrchr(out, '/');
  if (!slash)
    return false;
  slash[1] = 0;
  return true;
}

// gtaiv_dxvk_vr.caliblum: 1 = also measure per-eye luminance (GPU readback).
// Absent / 0 = off, so the first probe build costs nothing but log lines.
bool LumaEnabled() {
  static std::atomic<bool> s_read{false};
  static std::atomic<bool> s_on{false};
  if (s_read.exchange(true))
    return s_on.load();
  char path[MAX_PATH]{};
  if (!GetAsiDirLocal(path, MAX_PATH))
    return false;
  strcat_s(path, "gtaiv_dxvk_vr.caliblum");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return false;
  char buf[16]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  int v = 0;
  if (n > 0 && sscanf_s(buf, "%d", &v) == 1 && v == 1) {
    s_on.store(true);
    Log("StereoCalib: luminance readback ENABLED (gtaiv_dxvk_vr.caliblum=1) — probe only");
  }
  return s_on.load();
}

struct CamBasis {
  float c[3];  // world eye position of the pass currently rendering
  float r[3];  // camera right
  float u[3];  // camera up
  float f[3];  // camera forward
};

// BuildLiveViewMatrix16 returns a row-major, row-vector D3D LH view matrix:
// columns 0/1/2 of the upper 3x3 are right / up / forward in world space.
bool GetLiveCamBasis(CamBasis* out) {
  float v[16];
  if (!out || !BuildLiveViewMatrix16(v))
    return false;
  if (!GetLastStereoCamPos(&out->c[0], &out->c[1], &out->c[2]))
    return false;
  out->r[0] = v[0];
  out->r[1] = v[4];
  out->r[2] = v[8];
  out->u[0] = v[1];
  out->u[1] = v[5];
  out->u[2] = v[9];
  out->f[0] = v[2];
  out->f[1] = v[6];
  out->f[2] = v[10];
  return true;
}

// layout 1 = row-vector (p' = p * M, memory rows are matrix rows)
// layout 2 = column-vector (p' = M * p)
// Same two interpretations the proven detector in stereo_proj.cpp uses.
void Transform4(const float* m, int layout, const float p[3], float out4[4]) {
  const float x = p[0], y = p[1], z = p[2];
  if (layout == 1) {
    out4[0] = x * m[0] + y * m[4] + z * m[8] + m[12];
    out4[1] = x * m[1] + y * m[5] + z * m[9] + m[13];
    out4[2] = x * m[2] + y * m[6] + z * m[10] + m[14];
    out4[3] = x * m[3] + y * m[7] + z * m[11] + m[15];
  } else {
    out4[0] = x * m[0] + y * m[1] + z * m[2] + m[3];
    out4[1] = x * m[4] + y * m[5] + z * m[6] + m[7];
    out4[2] = x * m[8] + y * m[9] + z * m[10] + m[11];
    out4[3] = x * m[12] + y * m[13] + z * m[14] + m[15];
  }
}

// Pre-filter: does this 4x4 block map the camera position onto the view axis
// (x' = y' = 0)? True for a view matrix and for a view-projection matrix, false for
// world/bone/shadow matrices. Mirrors stereo_proj.cpp:252-257.
bool MapsCamToAxis(const float* m, int layout, const float c[3]) {
  float q[4];
  Transform4(m, layout, c, q);
  float gx = 0.f, gy = 0.f;
  if (layout == 1) {
    gx = std::fabs(c[0] * m[0]) + std::fabs(c[1] * m[4]) + std::fabs(c[2] * m[8]) +
         std::fabs(m[12]);
    gy = std::fabs(c[0] * m[1]) + std::fabs(c[1] * m[5]) + std::fabs(c[2] * m[9]) +
         std::fabs(m[13]);
  } else {
    gx = std::fabs(c[0] * m[0]) + std::fabs(c[1] * m[1]) + std::fabs(c[2] * m[2]) +
         std::fabs(m[3]);
    gy = std::fabs(c[0] * m[4]) + std::fabs(c[1] * m[5]) + std::fabs(c[2] * m[6]) +
         std::fabs(m[7]);
  }
  return std::fabs(q[0]) < 2e-3f * gx + 1e-3f && std::fabs(q[1]) < 2e-3f * gy + 1e-3f &&
         (std::fabs(q[2]) > 1e-3f || std::fabs(q[3]) > 0.5f) && gx > 1e-3f;
}

struct Measurement {
  float tanH;
  float tanV;
  float centerTanH;  // frustum centre offset in tangent units (0 = symmetric)
  float centerTanV;
  float scaleH;      // signed 1/tanH as the engine produced it (sign = axis direction)
  float scaleV;
};

// Three world points instead of a matrix inverse: convention-free and exact.
//   Pc = c + f*d              -> ndc.x = a              (asymmetry offset)
//   Pr = c + f*d + r*d        -> ndc.x = 1/tanH + a     (x_view/z_view == 1)
//   Pu = c + f*d + u*d        -> ndc.y = 1/tanV + b
bool MeasureBlock(const float* m, int layout, const CamBasis& cb, Measurement* out) {
  const float d = kProbeDistM;
  float pc[3], pr[3], pu[3];
  for (int i = 0; i < 3; ++i) {
    pc[i] = cb.c[i] + cb.f[i] * d;
    pr[i] = pc[i] + cb.r[i] * d;
    pu[i] = pc[i] + cb.u[i] * d;
  }

  float qc[4], qr[4], qu[4];
  Transform4(m, layout, pc, qc);
  Transform4(m, layout, pr, qr);
  Transform4(m, layout, pu, qu);

  // A view-projection carries w = z_view; a plain view matrix carries w = 1.
  const float w = std::fabs(qc[3]);
  if (!(w > 0.25f * d) || !(std::fabs(w - d) < 0.25f * d))
    return false;
  if (!(std::fabs(qr[3]) > 1e-3f) || !(std::fabs(qu[3]) > 1e-3f))
    return false;

  const float ndcCx = qc[0] / qc[3];
  const float ndcCy = qc[1] / qc[3];
  const float ndcRx = qr[0] / qr[3];
  const float ndcUy = qu[1] / qu[3];

  const float sH = ndcRx - ndcCx;
  const float sV = ndcUy - ndcCy;
  if (!(std::fabs(sH) > 0.05f) || !(std::fabs(sV) > 0.05f))
    return false;

  out->scaleH = sH;
  out->scaleV = sV;
  out->tanH = 1.f / std::fabs(sH);
  out->tanV = 1.f / std::fabs(sV);
  out->centerTanH = -ndcCx / sH;
  out->centerTanV = -ndcCy / sV;
  if (!std::isfinite(out->tanH) || !std::isfinite(out->tanV))
    return false;
  return out->tanH > 0.1f && out->tanH < 8.f && out->tanV > 0.1f && out->tanV < 8.f;
}

float YawDegFromEyeToHead(const vr::HmdMatrix34_t& m) {
  return std::atan2(m.m[0][2], m.m[0][0]) * (180.f / kPi);
}

void LogStaticOnce() {
  float lL = 0.f, lR = 0.f, lT = 0.f, lB = 0.f;
  float rL = 0.f, rR = 0.f, rT = 0.f, rB = 0.f;
  const bool haveRaw = GetEyeRawProjection(vr::Eye_Left, &lL, &lR, &lT, &lB) &&
                       GetEyeRawProjection(vr::Eye_Right, &rL, &rR, &rT, &rB);
  float coverH = 0.f, coverV = 0.f;
  GetCoverFovTangents(&coverH, &coverV);
  const char* kind =
      IsTrueStereoLeftPublish(GetStereoMode())
          ? "LEFT-PUBLISH mode 236"
          : (IsTrueStereoStablePublish(GetStereoMode())
                 ? "STABLE-PUBLISH mode 235"
                 : (IsTrueStereoCover(GetStereoMode())
                        ? "COVER mode 232"
                        : (IsTrueStereoExact(GetStereoMode()) ? "EXACT mode 231"
                                                              : "PROBE mode 230")));
  const char* job =
      IsTrueStereoLeftPublish(GetStereoMode())
          ? "235 gate + publish gameTan from Left eye only"
          : (IsTrueStereoStablePublish(GetStereoMode())
                 ? "Direct + gate wild measTan before PUBLISH / fpfov raise"
                 : (IsTrueStereoCover(GetStereoMode())
                        ? "Exact publish + raise fpfov until measTan>=cover (§4.3)"
                        : (IsTrueStereoExact(GetStereoMode())
                               ? "measure + PUBLISH gameTan (no kEng / no under-publish)"
                               : "READ-ONLY (no write, no game hook)")));
  Log("StereoCalib: %s on 203 seam — %s. "
      "cover=(%.4f,%.4f) rawL=(%.4f,%.4f,%.4f,%.4f) rawR=(%.4f,%.4f,%.4f,%.4f) haveRaw=%d",
      kind, job, coverH, coverV, lL, lR, lT, lB, rL, rR, rT, rB, haveRaw ? 1 : 0);

  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys)
    return;
  const vr::HmdMatrix34_t eL = sys->GetEyeToHeadTransform(vr::Eye_Left);
  const vr::HmdMatrix34_t eR = sys->GetEyeToHeadTransform(vr::Eye_Right);
  const float ipdM = std::fabs(eR.m[0][3] - eL.m[0][3]);
  Log("StereoCalibHmd: eyeL=(%.4f,%.4f,%.4f) eyeR=(%.4f,%.4f,%.4f) ipd=%.1fmm "
      "cantL=%.2fdeg cantR=%.2fdeg (cant != 0 means a pure translation IPD is wrong) "
      "sepInUse=%.2fcm stereoscale=%.2f",
      eL.m[0][3], eL.m[1][3], eL.m[2][3], eR.m[0][3], eR.m[1][3], eR.m[2][3], ipdM * 1000.f,
      YawDegFromEyeToHead(eL), YawDegFromEyeToHead(eR), GetStereoSepMeters() * 100.f,
      GetStereoScale());
}

// Mode 235: reject wild StereoCalib samples before they flap Direct Submit bounds.
// Returns nullptr if OK; else a short reason tag for the REJECT log suffix.
const char* StablePublishRejectReason(const Measurement& ms, float lastGoodH, float lastGoodV,
                                      bool haveLastGood) {
  const float lo = (std::min)(ms.tanH, ms.tanV);
  const float hi = (std::max)(ms.tanH, ms.tanV);
  if (!(lo > 1e-4f) || hi / lo > 1.35f)
    return "aspect";
  if (ms.tanH < 0.50f || ms.tanV < 0.50f || ms.tanH > 2.50f || ms.tanV > 2.50f)
    return "range";
  if (std::fabs(ms.centerTanH) >= 0.15f || std::fabs(ms.centerTanV) >= 0.15f)
    return "center";
  if (haveLastGood) {
    if (lastGoodH > 1e-4f && std::fabs(ms.tanH - lastGoodH) > 0.25f * lastGoodH)
      return "jumpH";
    if (lastGoodV > 1e-4f && std::fabs(ms.tanV - lastGoodV) > 0.25f * lastGoodV)
      return "jumpV";
  }
  return nullptr;
}

void ReportMeasurement(int eyeIdx, int layout, unsigned reg, const float* m,
                       const Measurement& ms) {
  float pubH = 0.f, pubV = 0.f;
  GetGameFovTangents(&pubH, &pubV);
  float coverH = 0.f, coverV = 0.f;
  GetCoverFovTangents(&coverH, &coverV);

  const float errH = (pubH > 1e-4f) ? ((ms.tanH / pubH) - 1.f) * 100.f : 0.f;
  const float errV = (pubV > 1e-4f) ? ((ms.tanV / pubV) - 1.f) * 100.f : 0.f;

  // What the guessed kEng conversion would have claimed for the same CCam setting.
  const float ccamDeg = GetFpForwardFovDegrees();
  const float kEng = 58.7f / 45.f;
  float predV = std::tan(0.5f * ccamDeg * kEng * kPi / 180.f);
  const float aspect = GetBackbufferAspect();
  float predH = predV * aspect;
  if (!std::isfinite(predV) || predV < 0.f || predV > 99.f)
    predV = predH = 0.f;

  // Mode 231+: publish measured tangents as gameTan (Exact / Cover inherit).
  // Mode 235: gate first — REJECT keeps last good gameTan (no publish).
  // Log err* against the PREVIOUS published value first so the line still shows the
  // correction that was applied.
  static float s_lastGoodH = 0.f;
  static float s_lastGoodV = 0.f;
  static bool s_haveLastGood = false;

  const bool wantPublish = IsTrueStereoExact(GetStereoMode());
  const bool gate = IsTrueStereoStablePublish(GetStereoMode());
  const char* rejectWhy = nullptr;
  if (wantPublish && IsTrueStereoLeftPublish(GetStereoMode()) && eyeIdx != 0)
    rejectWhy = "eyeR";
  else if (wantPublish && gate)
    rejectWhy = StablePublishRejectReason(ms, s_lastGoodH, s_lastGoodV, s_haveLastGood);
  const bool publish = wantPublish && !rejectWhy;
  if (publish) {
    PublishGameFovMeasured(ms.tanH, ms.tanV);
    g_haveMeasurement.store(true);
    s_lastGoodH = ms.tanH;
    s_lastGoodV = ms.tanV;
    s_haveLastGood = true;
  }

  // Mode 232 Cover (§4.3): raise engine fpfov in steps until measTan >= coverTan.
  // Mode 235: never raise from a REJECT sample (garbage under-cover drove fpfov→120).
  const bool coverMode = IsTrueStereoCover(GetStereoMode());
  if (coverMode && !rejectWhy && coverH > 0.05f && coverV > 0.05f) {
    if (ms.tanH < coverH * 0.98f || ms.tanV < coverV * 0.98f) {
      const int cur = static_cast<int>(GetFpForwardFovDegrees() + 0.5f);
      if (cur < 120) {
        const int next = (std::min)(cur + 5, 120);
        ForceFpFovDegrees(next, next, next);
        Log("Mode232: under-cover meas=(%.3f,%.3f) cover=(%.3f,%.3f) — raised fpfov %d→%d",
            ms.tanH, ms.tanV, coverH, coverV, cur, next);
      } else {
        static bool s_clamp = false;
        if (!s_clamp) {
          s_clamp = true;
          Log("Mode232: under-cover but fpfov already 120 — accept peripheral bars "
              "(correct geometry beats a full field)");
        }
      }
    } else {
      static bool s_ok = false;
      if (!s_ok) {
        s_ok = true;
        Log("Mode232: cover OK meas=(%.3f,%.3f) >= cover=(%.3f,%.3f) — no fpfov raise",
            ms.tanH, ms.tanV, coverH, coverV);
      }
    }
  }

  // §4.3: effective pixels-per-degree (square BB trade when cropping to cover).
  const float coverDegH =
      (coverH > 1e-4f) ? (2.f * std::atan(coverH) * 180.f / kPi) : 0.f;
  const float ppd = (coverDegH > 1.f) ? (2560.f / coverDegH) : 0.f;

  const unsigned n = ++g_samples;
  const char* tag = publish ? " PUBLISH" : (rejectWhy ? " REJECT" : "");
  Log("StereoCalib: #%u eye=%s layout=%s reg=%u measTan=(%.4f,%.4f) pubTan=(%.4f,%.4f) "
      "errH=%+.1f%% errV=%+.1f%% center=(%+.4f,%+.4f) cover=(%.4f,%.4f) "
      "coverFill=%.0f%%h/%.0f%%v ccamFov=%.1f kEngWouldSay=(%.4f,%.4f) sign=(%+.0f,%+.0f) "
      "ppd≈%.1f%s%s%s",
      n, eyeIdx == 1 ? "R" : "L", layout == 1 ? "row" : "col", reg, ms.tanH, ms.tanV, pubH,
      pubV, errH, errV, ms.centerTanH, ms.centerTanV, coverH, coverV,
      (coverH > 1e-4f) ? (ms.tanH / coverH) * 100.f : 0.f,
      (coverV > 1e-4f) ? (ms.tanV / coverV) * 100.f : 0.f, ccamDeg, predH, predV,
      ms.scaleH < 0.f ? -1.f : 1.f, ms.scaleV < 0.f ? -1.f : 1.f, ppd, tag,
      rejectWhy ? " reason=" : "", rejectWhy ? rejectWhy : "");

  if (!g_loggedRawBlock) {
    g_loggedRawBlock = true;
    Log("StereoCalibRaw: block reg=%u [%.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | "
        "%.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f]",
        reg, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12],
        m[13], m[14], m[15]);
  }
}

void ReleaseLumaSurfaces() {
  if (g_lumaRt) {
    g_lumaRt->Release();
    g_lumaRt = nullptr;
  }
  if (g_lumaSys) {
    g_lumaSys->Release();
    g_lumaSys = nullptr;
  }
}

bool EnsureLumaSurfaces(IDirect3DDevice9* dev) {
  if (g_lumaRt && g_lumaSys)
    return true;
  ReleaseLumaSurfaces();
  if (FAILED(dev->CreateRenderTarget(kLumaDim, kLumaDim, D3DFMT_A8R8G8B8,
                                     D3DMULTISAMPLE_NONE, 0, FALSE, &g_lumaRt, nullptr)) ||
      !g_lumaRt) {
    g_lumaRt = nullptr;
    return false;
  }
  if (FAILED(dev->CreateOffscreenPlainSurface(kLumaDim, kLumaDim, D3DFMT_A8R8G8B8,
                                              D3DPOOL_SYSTEMMEM, &g_lumaSys, nullptr)) ||
      !g_lumaSys) {
    ReleaseLumaSurfaces();
    return false;
  }
  Log("StereoCalib: luma readback surfaces %ux%u created", kLumaDim, kLumaDim);
  return true;
}

bool MeanLuma(IDirect3DDevice9* dev, IDirect3DTexture9* tex, float* out) {
  IDirect3DSurface9* src = nullptr;
  if (FAILED(tex->GetSurfaceLevel(0, &src)) || !src)
    return false;
  const bool blit = SUCCEEDED(dev->StretchRect(src, nullptr, g_lumaRt, nullptr, D3DTEXF_LINEAR));
  src->Release();
  if (!blit)
    return false;
  if (FAILED(dev->GetRenderTargetData(g_lumaRt, g_lumaSys)))
    return false;

  D3DLOCKED_RECT lr{};
  if (FAILED(g_lumaSys->LockRect(&lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits)
    return false;
  double sum = 0.0;
  for (unsigned y = 0; y < kLumaDim; ++y) {
    const BYTE* row = static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
    for (unsigned x = 0; x < kLumaDim; ++x) {
      const BYTE* px = row + static_cast<size_t>(x) * 4u;  // A8R8G8B8 = B,G,R,A in memory
      sum += 0.0722 * px[0] + 0.7152 * px[1] + 0.2126 * px[2];
    }
  }
  g_lumaSys->UnlockRect();
  *out = static_cast<float>(sum / (kLumaDim * kLumaDim));
  return true;
}

}  // namespace

bool StereoCalibWantsVsProbe() {
  return g_arm.load();
}

bool StereoCalibHasMeasurement() {
  return g_haveMeasurement.load();
}

void StereoCalibOnVsConst(unsigned startRegister, const float* data, unsigned vec4Count) {
  if (!g_arm.load())
    return;
  if (!IsMeasureMode()) {
    g_arm.store(false);  // self-heal if the mode changed without an EndScene tick
    return;
  }
  if (!data || vec4Count < 4 || vec4Count > 256)
    return;
  const int eyeIdx = (GetStereoEye() == StereoEye::Right) ? 1 : 0;
  if (g_eyeDone[eyeIdx])
    return;
  g_uploadsSeen.fetch_add(1);

  CamBasis cb;
  if (!GetLiveCamBasis(&cb)) {
    g_camBasisFails.fetch_add(1);
    return;
  }

  for (unsigned b = 0; b + 4u <= vec4Count; b += 4u) {
    const float* m = data + static_cast<size_t>(b) * 4u;
    for (int layout = 1; layout <= 2; ++layout) {
      if (!MapsCamToAxis(m, layout, cb.c))
        continue;
      g_axisMatches.fetch_add(1);
      Measurement ms{};
      if (!MeasureBlock(m, layout, cb, &ms))
        continue;  // matched the view matrix, not the view-projection — keep looking
      ReportMeasurement(eyeIdx, layout, startRegister + b, m, ms);
      g_eyeDone[eyeIdx] = true;
      if (g_eyeDone[0] && g_eyeDone[1])
        g_arm.store(false);
      return;
    }
  }
}

void StereoCalibOnEndScene() {
  if (!IsMeasureMode()) {
    g_arm.store(false);
    if (!IsTrueStereoExact(GetStereoMode()) && !IsProbeMode())
      g_haveMeasurement.store(false);
    return;
  }
  const unsigned n = ++g_pairs;
  if (!g_loggedStatic) {
    g_loggedStatic = true;
    LogStaticOnce();
  }
  if ((n % kSampleEveryPairs) != 1u)
    return;

  // A silent probe is useless — say why nothing was measured.
  if (n > 1u && !g_eyeDone[0] && !g_eyeDone[1]) {
    ++g_missStreak;
    if (g_missStreak == 3u && !g_loggedMiss) {
      g_loggedMiss = true;
      Log("StereoCalib: NO viewProj block matched in 3 armed pairs — uploads=%u "
          "axisMatches=%u camBasisFails=%u. axisMatches=0 means our baked camera is not "
          "what the shaders use; axisMatches>0 means only the view matrix was found and "
          "the viewProj never passes through SetVertexShaderConstantF.",
          g_uploadsSeen.load(), g_axisMatches.load(), g_camBasisFails.load());
    }
  } else if (g_eyeDone[0] || g_eyeDone[1]) {
    g_missStreak = 0;
  }

  g_eyeDone[0] = false;
  g_eyeDone[1] = false;
  g_arm.store(true);
}

void StereoCalibSampleEyeLuma(IDirect3DDevice9* device, IDirect3DTexture9* texLeft,
                              IDirect3DTexture9* texRight) {
  // Probe + SameState (§4.5 needs L/R luminance numbers).
  const bool want = IsProbeMode() || IsTrueStereoSameState(GetStereoMode());
  if (!want || !device || !texLeft || !texRight)
    return;
  if (IsProbeMode() && !LumaEnabled())
    return;
  static unsigned s_n = 0;
  if ((++s_n % kSampleEveryPairs) != 0u)
    return;
  if (!EnsureLumaSurfaces(device))
    return;
  float lumaL = 0.f, lumaR = 0.f;
  if (!MeanLuma(device, texLeft, &lumaL) || !MeanLuma(device, texRight, &lumaR))
    return;
  const float delta = (lumaL > 1e-3f) ? ((lumaR / lumaL) - 1.f) * 100.f : 0.f;
  Log("StereoCalibLuma: L=%.1f R=%.1f delta=%+.1f%% (systematic delta = auto-exposure "
      "runs between the two passes — see STEREO_TRUETICK_REVIEW.md §4.5)",
      lumaL, lumaR, delta);
}

}  // namespace asi
