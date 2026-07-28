#include "stereo_dual.h"
#include "stereo_config.h"
#include "hmd_pose.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>

namespace asi {
namespace {

std::atomic<bool> g_holdActive{false};
std::atomic<bool> g_liveLookActive{false};
std::atomic<uint32_t> g_everyN{0};
std::atomic<bool> g_loggedN{false};
std::atomic<int> g_poseHoldTicks{0};
std::atomic<uint32_t> g_lookHoldN{0};
std::atomic<uint32_t> g_poseHoldN{0};
std::atomic<uint32_t> g_liveLookN{0};
std::atomic<uint32_t> g_forceDualN{0};
std::atomic<uint32_t> g_poseMonoN{0};
std::atomic<DWORD> g_poseMonoUntil{0};
std::atomic<uint32_t> g_monoLookN{0};
std::atomic<DWORD> g_monoLookUntil{0};
std::atomic<uint32_t> g_monoLookPitchN{0};
std::atomic<DWORD> g_monoLookPitchUntil{0};
std::atomic<int> g_monoLookPitchSustain{0};
std::atomic<uint32_t> g_alwaysFreshMonoN{0};
std::atomic<DWORD> g_alwaysFreshMonoUntil{0};

bool GetAsiDir(char* out, DWORD outLen) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&GetAsiDir), &self))
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

// OpenVR forward ≈ −Z column of HmdMatrix34. Yaw around +Y; pitch around +X.
void HmdYawPitchDeg(const vr::HmdMatrix34_t& m, float* outYaw, float* outPitch) {
  // Forward vector in OpenVR: (−m[0][2], −m[1][2], −m[2][2]) for −Z column.
  const float fx = -m.m[0][2];
  const float fy = -m.m[1][2];
  const float fz = -m.m[2][2];
  if (outYaw)
    *outYaw = std::atan2(fx, fz) * (180.f / 3.14159265f);
  if (outPitch) {
    const float horiz = std::sqrt(fx * fx + fz * fz);
    *outPitch = std::atan2(fy, horiz) * (180.f / 3.14159265f);
  }
}

float AngleDeltaDeg(float a, float b) {
  float d = a - b;
  while (d > 180.f)
    d -= 360.f;
  while (d < -180.f)
    d += 360.f;
  return d;
}

bool ConsumePoseHoldTick() {
  if (g_poseHoldTicks.load() <= 0)
    return false;
  g_poseHoldTicks.fetch_sub(1);
  return true;
}

bool LookRateOverThreshold(float* outDyaw, float* outDpitch) {
  vr::HmdMatrix34_t m{};
  if (!GetHmdPoseMatrix(&m))
    return false;

  static bool s_have = false;
  static float s_prevYaw = 0.f;
  static float s_prevPitch = 0.f;
  float yaw = 0.f, pitch = 0.f;
  HmdYawPitchDeg(m, &yaw, &pitch);
  if (!s_have) {
    s_prevYaw = yaw;
    s_prevPitch = pitch;
    s_have = true;
    return false;
  }
  const float dyaw = std::fabs(AngleDeltaDeg(yaw, s_prevYaw));
  const float dpitch = std::fabs(AngleDeltaDeg(pitch, s_prevPitch));
  s_prevYaw = yaw;
  s_prevPitch = pitch;
  if (outDyaw)
    *outDyaw = dyaw;
  if (outDpitch)
    *outDpitch = dpitch;
  // Fast look (yaw or pitch): skip DrawScene×2 / force dual (Mode 129).
  return dyaw >= 1.25f || dpitch >= 1.0f;
}

// Mode 131: pitch noise is jumpy — require higher pitch Δ than yaw, and sustain.
bool LookRateOverThresholdPitchStable(float* outDyaw, float* outDpitch, bool* outSustained) {
  if (outSustained)
    *outSustained = false;
  vr::HmdMatrix34_t m{};
  if (!GetHmdPoseMatrix(&m))
    return false;

  static bool s_have = false;
  static float s_prevYaw = 0.f;
  static float s_prevPitch = 0.f;
  float yaw = 0.f, pitch = 0.f;
  HmdYawPitchDeg(m, &yaw, &pitch);
  if (!s_have) {
    s_prevYaw = yaw;
    s_prevPitch = pitch;
    s_have = true;
    g_monoLookPitchSustain.store(0);
    return false;
  }
  const float dyaw = std::fabs(AngleDeltaDeg(yaw, s_prevYaw));
  const float dpitch = std::fabs(AngleDeltaDeg(pitch, s_prevPitch));
  s_prevYaw = yaw;
  s_prevPitch = pitch;
  if (outDyaw)
    *outDyaw = dyaw;
  if (outDpitch)
    *outDpitch = dpitch;
  // Yaw: 1.25° (same as 130). Pitch: 2.5° (Mode130 used 1.0° → flicker on nod).
  const bool over = dyaw >= 1.25f || dpitch >= 2.5f;
  if (over) {
    const int n = g_monoLookPitchSustain.load() + 1;
    g_monoLookPitchSustain.store(n);
    // Require 3 consecutive over-threshold frames before entering mono.
    if (outSustained)
      *outSustained = (n >= 3);
    return true;
  }
  g_monoLookPitchSustain.store(0);
  return false;
}

}  // namespace

bool StereoDualHoldActive() {
  return g_holdActive.load();
}

void StereoDualClearHold() {
  g_holdActive.store(false);
}

void StereoDualMarkHold() {
  g_holdActive.store(true);
}

bool StereoDualLiveLookActive() {
  return g_liveLookActive.load();
}

void StereoDualClearLiveLook() {
  g_liveLookActive.store(false);
}

void StereoDualMarkLiveLook() {
  g_liveLookActive.store(true);
}

uint32_t StereoDualHitchMs() {
  // Mode 127–135: tighter hitch (skip dual earlier). Mode 122/126: 40ms.
  if (IsCleanDualFpsPoseTight(GetStereoMode()))
    return 32u;
  if (IsCleanDualPerfLookHold(GetStereoMode()) || IsCleanDualFpsLiveLook(GetStereoMode()))
    return 40u;
  return 50u;
}

uint32_t StereoDualEveryN() {
  // Mode 168: always everyN=1 (ignore cached dualn file / prior mode).
  if (IsOursFpFlashStable(GetStereoMode())) {
    if (!g_loggedN.exchange(true))
      Log("StereoDual: everyN=1 (Mode168 STABLE; no off-tick HOLD; dual every DrawScene)");
    return 1u;
  }
  uint32_t n = g_everyN.load();
  if (n == 0) {
    const StereoMode mode = GetStereoMode();
    // Mode 135: force everyN=1 — never off-tick HOLD of stale L/R.
    if (IsCleanDualAlwaysFresh(mode)) {
      n = 1;
      g_everyN.store(n);
      if (!g_loggedN.exchange(true))
        Log("StereoDual: everyN=1 (Mode135 ALWAYS-FRESH; no off-tick HOLD; dual every frame)");
      return n;
    }
    // Mode 122: force dualn=4 (lightest dual / most HOLD). Mode 121: force 3.
    // Mode 127–134: force 3 (tighter POSEHOLD needs more baseline HOLD headroom).
    // Mode 120 / 123 / 124 / 126: file or default 2 (LKG cadence).
    if (mode == StereoMode::CleanDualPerfLookHold) {
      n = 4;
      g_everyN.store(n);
      if (!g_loggedN.exchange(true))
        Log("StereoDual: everyN=4 (Mode122 force; look/pose HOLD + light dual)");
      return n;
    }
    if (mode == StereoMode::CleanDualPerfFillCombo) {
      n = 4;
      g_everyN.store(n);
      if (!g_loggedN.exchange(true))
        Log("StereoDual: everyN=4 (Mode125 force; LOOKHOLD+FILLMATCH combo)");
      return n;
    }
    if (mode == StereoMode::CleanDualLookMoveDualN3 || IsCleanDualFpsPoseTight(mode)) {
      n = 3;
      g_everyN.store(n);
      if (!g_loggedN.exchange(true)) {
        if (mode == StereoMode::CleanDualMonoLookBack132)
          Log("StereoDual: everyN=3 (Mode134 FAILED HOLD freeze; prefer 135)");
        else if (mode == StereoMode::CleanDualMonoLookHmdCheap)
          Log("StereoDual: everyN=3 (Mode133 FAILED same-tex; prefer 134/135)");
        else if (mode == StereoMode::CleanDualMonoLookHmdEvery)
          Log("StereoDual: everyN=3 (Mode132 force; calm HOLD; pitch monolook every-frame BB)");
        else if (mode == StereoMode::CleanDualMonoLookPitch)
          Log("StereoDual: everyN=3 (Mode131 force; calm HOLD; pitch-stable monolook)");
        else if (mode == StereoMode::CleanDualMonoLook)
          Log("StereoDual: everyN=3 (Mode130 force; calm HOLD; look→mono BB hyst)");
        else if (mode == StereoMode::CleanDualLrSync)
          Log("StereoDual: everyN=3 (Mode129 force; calm HOLD; look→force dual)");
        else if (mode == StereoMode::CleanDualFpsHoldStereo)
          Log("StereoDual: everyN=3 (Mode128 force; POSEHOLD tight + HOLD last stereo)");
        else if (mode == StereoMode::CleanDualFpsPoseTight)
          Log("StereoDual: everyN=3 (Mode127 force; POSEHOLD tight + LIVELOOK)");
        else
          Log("StereoDual: everyN=3 (Mode121 force; lighter HOLD experiment)");
      }
      return n;
    }
    n = 2;
    char path[MAX_PATH]{};
    if (GetAsiDir(path, MAX_PATH)) {
      strcat_s(path, "gtaiv_dxvk_vr.dualn");
      FILE* f = nullptr;
      if (fopen_s(&f, path, "rb") == 0 && f) {
        char buf[8]{};
        const size_t len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        int v = 0;
        if (len > 0 && sscanf_s(buf, "%d", &v) == 1 && v >= 2 && v <= 4)
          n = static_cast<uint32_t>(v);
      }
    }
    g_everyN.store(n);
    if (!g_loggedN.exchange(true)) {
      if (mode == StereoMode::CleanDualFpsLiveLook)
        Log("StereoDual: everyN=%u (Mode126; dualn file/default 2; POSEHOLD+LIVELOOK)", n);
      else if (mode == StereoMode::CleanDualLateLatch)
        Log("StereoDual: everyN=%u (Mode136 LATE-LATCH; dualn file/default 2; Mode120 HOLD)", n);
      else
        Log("StereoDual: everyN=%u (gtaiv_dxvk_vr.dualn; Mode120/123/124 default 2)", n);
    }
  }
  return n;
}

uint32_t StereoDualGateNeed() {
  return 120u;  // ~4s settle before first DrawScene×2
}

void StereoDualNotePoseMs(double poseMs) {
  // Mode 122/125: stale HOLD. Mode 126/127: LIVELOOK. Mode 128: HOLD last stereo.
  // Mode 129: pose → mono BB with hysteresis. Mode 130: pose extends look-mono hyst.
  // Mode 131/132/133/134: pose → calm HOLD last stereo (not mono enter); look path separate.
  // Mode 135: pose → AlwaysFreshMono (fresh BB both eyes; never bare HOLD).
  if (!IsCleanDualPerfLookHold(GetStereoMode()) && !IsCleanDualFpsLiveLook(GetStereoMode()) &&
      !IsCleanDualFpsHoldStereo(GetStereoMode()) && !IsCleanDualLrSync(GetStereoMode()) &&
      !IsCleanDualMonoLook(GetStereoMode()) && !IsCleanDualMonoLookPitch(GetStereoMode()) &&
      !IsCleanDualMonoLookHmdEvery(GetStereoMode()) && !IsCleanDualMonoLookHmdCheap(GetStereoMode()) &&
      !IsCleanDualAlwaysFresh(GetStereoMode()))
    return;
  const bool tight = IsCleanDualFpsPoseTight(GetStereoMode());
  // Mode126: ≥16ms / 3 duals; ≥40ms HARD / 6.
  // Mode127–135 (log: avg stall ~21ms, HARD almost never): ≥12ms / 5; ≥24ms HARD / 8.
  const double hardMs = tight ? 24.0 : 40.0;
  const double softMs = tight ? 12.0 : 16.0;
  const int hardTicks = tight ? 8 : 6;
  const int softTicks = tight ? 5 : 3;
  if (poseMs >= hardMs) {
    g_poseHoldTicks.store(hardTicks);
    const uint32_t n = ++g_poseHoldN;
    if (n <= 8 || (n % 60) == 0)
      Log("Mode%d: POSEHOLD HARD poseMs=%.1f (skip %d duals; n=%u)",
          static_cast<int>(GetStereoMode()), poseMs, hardTicks, n);
  } else if (poseMs >= softMs) {
    g_poseHoldTicks.store(softTicks);
    const uint32_t n = ++g_poseHoldN;
    if (n <= 8 || (n % 120) == 0)
      Log("Mode%d: POSEHOLD poseMs=%.1f (skip %d duals; n=%u)",
          static_cast<int>(GetStereoMode()), poseMs, softTicks, n);
  }
}

bool StereoDualShouldLookHold() {
  // Mode 122/125 only — stale L/R HOLD (caused snap on look; Mode126/127 uses LIVELOOK).
  if (!IsCleanDualPerfLookHold(GetStereoMode()))
    return false;

  if (ConsumePoseHoldTick())
    return true;

  float dyaw = 0.f, dpitch = 0.f;
  if (!LookRateOverThreshold(&dyaw, &dpitch))
    return false;
  const uint32_t n = ++g_lookHoldN;
  if (n <= 8 || (n % 180) == 0)
    Log("Mode122: LOOKHOLD dyaw=%.2f dpitch=%.2f (HOLD; n=%u)", dyaw, dpitch, n);
  return true;
}

bool StereoDualShouldLiveLook() {
  // Mode 126/127: skip dual on pose stall or fast look → live BB both eyes (no stale HOLD).
  // Mode 128/129 deliberately excluded from look-rate LIVELOOK.
  if (!IsCleanDualFpsLiveLook(GetStereoMode()))
    return false;

  if (ConsumePoseHoldTick()) {
    const uint32_t n = ++g_liveLookN;
    if (n <= 8 || (n % 120) == 0)
      Log("Mode%d: LIVELOOK pose-budget (live BB both eyes; n=%u)",
          static_cast<int>(GetStereoMode()), n);
    return true;
  }

  float dyaw = 0.f, dpitch = 0.f;
  if (!LookRateOverThreshold(&dyaw, &dpitch))
    return false;
  const uint32_t n = ++g_liveLookN;
  if (n <= 8 || (n % 180) == 0)
    Log("Mode%d: LIVELOOK dyaw=%.2f dpitch=%.2f (live BB both eyes; n=%u)",
        static_cast<int>(GetStereoMode()), dyaw, dpitch, n);
  return true;
}

bool StereoDualShouldPoseHoldStereo() {
  // Mode 128: pose budget only → HOLD last stereo (skip dual; no LIVELOOK mono).
  // Mode 131/132/133/134 calm: same (look uses monolook path instead — no mismatched L/R).
  // Mode 135: NEVER — pose budget goes to AlwaysFreshMono (fresh BB; never bare HOLD).
  if (!IsCleanDualFpsHoldStereo(GetStereoMode()) &&
      !IsCleanDualMonoLookPitch(GetStereoMode()) &&
      !IsCleanDualMonoLookHmdEvery(GetStereoMode()) &&
      !IsCleanDualMonoLookHmdCheap(GetStereoMode()))
    return false;
  if (!ConsumePoseHoldTick())
    return false;
  const uint32_t n = ++g_lookHoldN;
  if (n <= 8 || (n % 120) == 0) {
    if (IsCleanDualMonoLookBack132(GetStereoMode()))
      Log("Mode134: POSEHOLD-CALM (HOLD last L/R; skip dual; n=%u)", n);
    else if (IsCleanDualMonoLookHmdCheap(GetStereoMode()))
      Log("Mode133: POSEHOLD-CALM (HOLD last L/R; skip dual; n=%u)", n);
    else if (IsCleanDualMonoLookHmdEvery(GetStereoMode()))
      Log("Mode132: POSEHOLD-CALM (HOLD last L/R; skip dual; n=%u)", n);
    else if (IsCleanDualMonoLookPitch(GetStereoMode()))
      Log("Mode131: POSEHOLD-CALM (HOLD last L/R; skip dual; n=%u)", n);
    else
      Log("Mode128: POSEHOLD-STEREO (HOLD last L/R; skip dual; n=%u)", n);
  }
  return true;
}

bool StereoDualShouldPoseMonoHyst() {
  // Mode 129: high pose_ms → mono BB both eyes; stay mono ≥200ms (no stereo flicker).
  if (!IsCleanDualLrSync(GetStereoMode()))
    return false;

  const DWORD now = GetTickCount();
  if (ConsumePoseHoldTick()) {
    const DWORD until = now + 200u;
    const DWORD cur = g_poseMonoUntil.load();
    if (until > cur)
      g_poseMonoUntil.store(until);
    const uint32_t n = ++g_poseMonoN;
    if (n <= 8 || (n % 120) == 0)
      Log("Mode129: POSEMONO-HYST (BB both eyes; n=%u)", n);
    return true;
  }

  const DWORD until = g_poseMonoUntil.load();
  if (until != 0 && now < until)
    return true;
  if (until != 0)
    g_poseMonoUntil.store(0);
  return false;
}

bool StereoDualShouldForceLookDual() {
  // Mode 129: looking → force same-tick DrawScene×2 (synced L/R; no everyN HOLD desync).
  if (!IsCleanDualLrSync(GetStereoMode()))
    return false;
  float dyaw = 0.f, dpitch = 0.f;
  if (!LookRateOverThreshold(&dyaw, &dpitch))
    return false;
  const uint32_t n = ++g_forceDualN;
  if (n <= 8 || (n % 180) == 0)
    Log("Mode129: LOOK-FORCE-DUAL dyaw=%.2f dpitch=%.2f (same-tick L/R; n=%u)", dyaw,
        dpitch, n);
  return true;
}

bool StereoDualShouldMonoLookHyst() {
  // Mode 130: look and/or pose → identical BB both eyes; stay mono ≥350ms after settle
  // (kills diplopia from sequential L→R DrawScene while head turns; no mono↔stereo flicker).
  if (!IsCleanDualMonoLook(GetStereoMode()))
    return false;

  constexpr DWORD kHystMs = 350u;
  const DWORD now = GetTickCount();
  bool trigger = false;
  float dyaw = 0.f, dpitch = 0.f;
  const bool looking = LookRateOverThreshold(&dyaw, &dpitch);
  const bool poseBudget = ConsumePoseHoldTick();
  if (looking || poseBudget) {
    trigger = true;
    const DWORD until = now + kHystMs;
    const DWORD cur = g_monoLookUntil.load();
    if (until > cur)
      g_monoLookUntil.store(until);
  }

  const DWORD until = g_monoLookUntil.load();
  if (until != 0 && now < until) {
    if (trigger) {
      const uint32_t n = ++g_monoLookN;
      if (n <= 8 || (n % 120) == 0) {
        if (looking)
          Log("Mode130: MONOLOOK-HYST dyaw=%.2f dpitch=%.2f (BB both eyes ≥%ums; n=%u)",
              dyaw, dpitch, kHystMs, n);
        else
          Log("Mode130: MONOLOOK-HYST pose-budget (BB both eyes ≥%ums; n=%u)", kHystMs, n);
      }
    }
    return true;
  }
  if (until != 0)
    g_monoLookUntil.store(0);
  return false;
}

bool StereoDualShouldMonoLookPitchHyst() {
  // Mode 131/132/133/134: sustained look → identical BB both eyes; ≥600ms hyst after settle.
  // Pitch thresh 2.5° (yaw 1.25°); require 3 over-threshold frames before enter.
  // Pose alone does NOT enter mono (calm → POSEHOLD-CALM last stereo).
  // Mode 131 EndScene: BB every 3rd (FAILED HMD starve). Mode 132/134: BB→L+R every.
  // Mode 133: BB→L only + Submit same tex L+R every (FAILED fusion/jump).
  if (!IsCleanDualMonoLookPitch(GetStereoMode()) &&
      !IsCleanDualMonoLookHmdEvery(GetStereoMode()) &&
      !IsCleanDualMonoLookHmdCheap(GetStereoMode()))
    return false;

  constexpr DWORD kHystMs = 600u;
  const DWORD now = GetTickCount();
  float dyaw = 0.f, dpitch = 0.f;
  bool sustained = false;
  const bool looking = LookRateOverThresholdPitchStable(&dyaw, &dpitch, &sustained);
  const DWORD curUntil = g_monoLookPitchUntil.load();
  const bool inMono = (curUntil != 0 && now < curUntil);

  // Enter only on sustained look; while already mono, any over-threshold extends.
  if ((sustained && looking) || (inMono && looking)) {
    const DWORD until = now + kHystMs;
    if (until > curUntil)
      g_monoLookPitchUntil.store(until);
  }

  const DWORD until = g_monoLookPitchUntil.load();
  if (until != 0 && now < until) {
    if (looking && sustained) {
      const uint32_t n = ++g_monoLookPitchN;
      if (n <= 8 || (n % 120) == 0) {
        if (IsCleanDualMonoLookBack132(GetStereoMode()))
          Log("Mode134: MONOLOOK-HMD dyaw=%.2f dpitch=%.2f (BB both EVERY ≥%ums; n=%u)",
              dyaw, dpitch, kHystMs, n);
        else if (IsCleanDualMonoLookHmdCheap(GetStereoMode()))
          Log("Mode133: MONOLOOK-CHEAP dyaw=%.2f dpitch=%.2f (BB→L + same Submit ≥%ums; n=%u)",
              dyaw, dpitch, kHystMs, n);
        else if (GetStereoMode() == StereoMode::CleanDualMonoLookHmdEvery)
          Log("Mode132: MONOLOOK-HMD dyaw=%.2f dpitch=%.2f (BB both EVERY ≥%ums; n=%u)",
              dyaw, dpitch, kHystMs, n);
        else
          Log("Mode131: MONOLOOK-PITCH dyaw=%.2f dpitch=%.2f (BB both ≥%ums; n=%u)",
              dyaw, dpitch, kHystMs, n);
      }
    }
    return true;
  }
  if (until != 0)
    g_monoLookPitchUntil.store(0);
  return false;
}

bool StereoDualShouldAlwaysFreshMono() {
  // Mode 135: look (LOW thresh) OR pose budget → identical BB both eyes every EndScene.
  // Never bare HOLD of previous-frame L/R. Calm (no look/pose) → everyN=1 dual instead.
  if (!IsCleanDualAlwaysFresh(GetStereoMode()))
    return false;

  constexpr DWORD kHystMs = 400u;
  const DWORD now = GetTickCount();

  // Dedicated low look thresh (Mode134 uses 1.25°/2.5°+sustain3 — too late for freeze).
  vr::HmdMatrix34_t m{};
  float dyaw = 0.f, dpitch = 0.f;
  bool looking = false;
  if (GetHmdPoseMatrix(&m)) {
    static bool s_have = false;
    static float s_prevYaw = 0.f, s_prevPitch = 0.f;
    float yaw = 0.f, pitch = 0.f;
    HmdYawPitchDeg(m, &yaw, &pitch);
    if (!s_have) {
      s_prevYaw = yaw;
      s_prevPitch = pitch;
      s_have = true;
    } else {
      dyaw = std::fabs(AngleDeltaDeg(yaw, s_prevYaw));
      dpitch = std::fabs(AngleDeltaDeg(pitch, s_prevPitch));
      s_prevYaw = yaw;
      s_prevPitch = pitch;
      looking = dyaw >= 0.75f || dpitch >= 0.75f;  // low — cover motion before stale HOLD
    }
  }
  const bool poseBudget = ConsumePoseHoldTick();
  const bool trigger = looking || poseBudget;
  if (trigger) {
    const DWORD until = now + kHystMs;
    const DWORD cur = g_alwaysFreshMonoUntil.load();
    if (until > cur)
      g_alwaysFreshMonoUntil.store(until);
  }

  const DWORD until = g_alwaysFreshMonoUntil.load();
  if (until != 0 && now < until) {
    if (trigger) {
      const uint32_t n = ++g_alwaysFreshMonoN;
      if (n <= 8 || (n % 120) == 0) {
        if (looking)
          Log("Mode135: MONOLOOK-FRESH dyaw=%.2f dpitch=%.2f (BB both EVERY ≥%ums; n=%u)",
              dyaw, dpitch, kHystMs, n);
        else
          Log("Mode135: POSE→FRESH-MONO (BB both EVERY; never bare HOLD; n=%u)", n);
      }
    }
    return true;
  }
  if (until != 0)
    g_alwaysFreshMonoUntil.store(0);
  return false;
}

}  // namespace asi
