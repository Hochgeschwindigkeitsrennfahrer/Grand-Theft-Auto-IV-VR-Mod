#include "stereo_render.h"
#include "aob.h"
#include "cam_matrix.h"
#include "game_timer.h"
#include "hmd_pose.h"
#include "log.h"
#include "perf_debug.h"
#include "stereo_config.h"
#include "stereo_dual.h"
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

// Mode 197 COUNT-only: file-scope for naked asm (stack-arg target 0x220D0 — not thiscall).
static void* g_asi_mode197_tramp = nullptr;
static volatile LONG g_asi_mode197_entries = 0;
static volatile LONG g_asi_mode197_frame = 0;

extern "C" __declspec(naked) void AsiHookMode197Count() {
  __asm {
    lock inc dword ptr [g_asi_mode197_entries]
    lock inc dword ptr [g_asi_mode197_frame]
    jmp dword ptr [g_asi_mode197_tramp]
  }
}

// Mode 199 COUNT-only: fn-start 0x4D8BF0 (thiscall-56). Naked = ABI-safe count.
static void* g_asi_mode199_tramp = nullptr;
static volatile LONG g_asi_mode199_entries = 0;
static volatile LONG g_asi_mode199_frame = 0;

extern "C" __declspec(naked) void AsiHookMode199Count() {
  __asm {
    lock inc dword ptr [g_asi_mode199_entries]
    lock inc dword ptr [g_asi_mode199_frame]
    jmp dword ptr [g_asi_mode199_tramp]
  }
}

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
std::atomic<uint32_t> g_mode44RtChecks{0};
std::atomic<uint32_t> g_mode44RtRecreates{0};
std::atomic<uint32_t> g_mode44RtSuppressed{0};

bool g_haveL = false;
bool g_haveR = false;
// Mode 133 monolook: one BB→L StretchRect; StereoTrySubmitEyes submits g_texL for both eyes.
bool g_submitSameLBoth = false;
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
std::atomic<uint32_t> g_mode40MonoPairs{0};
std::atomic<uint32_t> g_stereoAuditPairs{0};
std::atomic<uint32_t> g_stereoAuditZero{0};
std::atomic<long long> g_stereoAuditDiffSum{0};

// Mode 38 AER: HMD pose at capture time (WaitGetPoses already ran this EndScene).
vr::HmdMatrix34_t g_holdPoseL{};
vr::HmdMatrix34_t g_holdPoseR{};
vr::HmdMatrix34_t g_submitPoseL{};
vr::HmdMatrix34_t g_submitPoseR{};
bool g_holdPoseLValid = false;
bool g_holdPoseRValid = false;
bool g_submitPoseLValid = false;
bool g_submitPoseRValid = false;

bool UsesFovComfortPath(StereoMode mode) {
  return mode == StereoMode::FovCanvasComfort || mode == StereoMode::AerPoseSubmit ||
         mode == StereoMode::FovCanvasLowMotion || mode == StereoMode::FovCanvasMotionGuard ||
         mode == StereoMode::ReplayCallChainProbe || mode == StereoMode::ReplayOwnerCountProbe ||
         mode == StereoMode::FovCanvasMotionGuardFast ||
         mode == StereoMode::FovCanvasMotionGuardRtLock ||
         mode == StereoMode::HeadOwnedCamSpike ||
         mode == StereoMode::HeadOwnedCamFullPose ||
         mode == StereoMode::HeadOwnedCamLeveledPitchFlip ||
         mode == StereoMode::HeadOwnedCamPitchStable ||
         mode == StereoMode::HeadOwnedCamPedCoupled ||
         mode == StereoMode::HeadOwnedCamStereoAlways ||
         mode == StereoMode::HeadOwnedCamStereoAer ||
         mode == StereoMode::HeadOwnedCamStereoSwap ||
         mode == StereoMode::HeadOwnedCamStereoSoftGuard ||
         UsesDrawSceneDualPath(mode);
}

bool UsesTrueFovCanvasPublish(StereoMode mode) {
  return mode == StereoMode::FovRecomputeTrueCanvas || UsesFovComfortPath(mode);
}

void SnapshotHoldPose(bool rightEye) {
  vr::HmdMatrix34_t m{};
  if (!GetHmdPoseMatrix(&m)) {
    if (rightEye)
      g_holdPoseRValid = false;
    else
      g_holdPoseLValid = false;
    return;
  }
  if (rightEye) {
    g_holdPoseR = m;
    g_holdPoseRValid = true;
  } else {
    g_holdPoseL = m;
    g_holdPoseLValid = true;
  }
}

// Mode 40 is deliberately NOT a synthetic same-frame stereo claim. The game
// rendered L and R on separate frames, but a rapid HMD move makes that stale-eye
// disparity much more objectionable than losing stereo for that one submitted
// pair. Re-canvas the CURRENT R frame into both eye textures instead.
bool ComputeHoldPoseDelta(float* outRotDeg, float* outMoveCm) {
  if (!g_holdPoseLValid || !g_holdPoseRValid)
    return false;
  const vr::HmdMatrix34_t& l = g_holdPoseL;
  const vr::HmdMatrix34_t& r = g_holdPoseR;
  const float trace = l.m[0][0] * r.m[0][0] + l.m[0][1] * r.m[0][1] +
                      l.m[0][2] * r.m[0][2] + l.m[1][0] * r.m[1][0] +
                      l.m[1][1] * r.m[1][1] + l.m[1][2] * r.m[1][2] +
                      l.m[2][0] * r.m[2][0] + l.m[2][1] * r.m[2][1] +
                      l.m[2][2] * r.m[2][2];
  const float cosAngle = (std::max)(-1.f, (std::min)(1.f, (trace - 1.f) * 0.5f));
  const float rotDeg = std::acos(cosAngle) * 57.2957795f;
  const float dx = r.m[0][3] - l.m[0][3];
  const float dy = r.m[1][3] - l.m[1][3];
  const float dz = r.m[2][3] - l.m[2][3];
  const float moveCm = std::sqrt(dx * dx + dy * dy + dz * dz) * 100.f;
  if (outRotDeg)
    *outRotDeg = rotDeg;
  if (outMoveCm)
    *outMoveCm = moveCm;
  return true;
}

bool Mode40NeedsMonoGuard(float* outRotDeg, float* outMoveCm) {
  const StereoMode mode = GetStereoMode();
  if (!UsesMotionGuardStereo(mode))
    return false;
  float rotDeg = 0.f, moveCm = 0.f;
  if (!ComputeHoldPoseDelta(&rotDeg, &moveCm))
    return false;
  if (outRotDeg)
    *outRotDeg = rotDeg;
  if (outMoveCm)
    *outMoveCm = moveCm;
  // One temporal pair normally spans two EndScene epochs. These thresholds
  // preserve stereo while still, but guard the visibly bad turn/lean case.
  return rotDeg >= 1.5f || moveCm >= 2.f;
}

// Mode 53: much higher thresholds than Mode 40; prefer distinct L/R + AER over flatten.
enum class SoftGuardAction : int { None = 0, AerDistinct = 1, FullMono = 2 };

SoftGuardAction Mode53SoftGuardAction(float rotDeg, float moveCm) {
  if (rotDeg >= 15.f || moveCm >= 12.f)
    return SoftGuardAction::FullMono;
  if (rotDeg >= 8.f || moveCm >= 6.f)
    return SoftGuardAction::AerDistinct;
  return SoftGuardAction::None;
}

std::atomic<uint32_t> g_mode53SoftGuard{0};
std::atomic<uint32_t> g_mode53FullMono{0};

// Mode 120: DrawScene×2 + HOLD state (see stereo_dual.cpp for cadence helpers).
std::atomic<bool> g_mode120DualDead{false};
std::atomic<uint32_t> g_mode120DualN{0};
// Mode193: after dual AV (door), skip DrawScene entirely — mono rebuild also AVs.
std::atomic<DWORD> g_mode193SkipDrawUntilMs{0};

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

// ---- Mode 41: read-only replay-thread call-chain probe -----------------------
// The Mode 34 stack trace identified only return addresses, which can be inside
// epilogues. Mode 41 preserves each address's stack slot and its direct CALL
// predecessor (when statically recoverable), then ranks it by EndScene epochs.
// No game-function hook or replay is attempted here.
struct Mode41FrameNode {
  uint32_t retRva;
  uint8_t slot;
  uint16_t hits;
};
struct Mode41AggNode {
  uint32_t retRva;
  uint32_t frames;
  uint32_t hits;
  uint8_t slot;
};
Mode41FrameNode g_mode41Frame[96]{};
int g_mode41FrameN = 0;
Mode41AggNode g_mode41Agg[96]{};
int g_mode41AggN = 0;
std::atomic<uint32_t> g_mode41VsRetHits{0};
std::atomic<uint32_t> g_mode41Es{0};
uint32_t g_mode41VsTid = 0;

// Mode 196: read-only resolve of Mode42 OWNER-EDGE call [eax+disp32] without
// invoking the target. Stack-scan for object candidates whose vtable+disp lands
// in executable GTAIV.exe code.
struct Mode196OwnerHit {
  uintptr_t obj;
  uintptr_t vtable;
  uint32_t targetRva;
  uint32_t hits;
};
Mode196OwnerHit g_mode196Owners[16]{};
int g_mode196OwnerN = 0;
std::atomic<uint32_t> g_mode196EdgeHits{0};
uint32_t g_mode196Disp = 0;
bool g_mode196HaveDisp = false;
uint8_t g_mode196Modrm = 0;
uint8_t g_mode196CallLen = 0;
uint32_t g_mode196CallRva = 0;

// Mode 197: COUNT-only naked hook on Mode196-proven target 0x220D0.
constexpr uint32_t kMode197TargetRva = 0x220D0;
void* g_mode197Target = nullptr;
std::atomic<uint32_t> g_mode197Es{0};
std::atomic<bool> g_mode197HookOk{false};

bool Mode41ForbiddenRva(uint32_t rva);
bool Mode34ForbiddenRva(uint32_t rva);

bool InstallMode197CountHook() {
  // Headset 2026-07-28: COUNT on 0x220D0 made FPS worse than 191 and amplified
  // flicker. Target is a per-draw leaf (Mode196 saw 10k–80k hits), not a frame
  // walker. Do not install — Mode197 render stays pure Mode191 dual.
  static bool s_once = false;
  if (!s_once) {
    s_once = true;
    Log("Mode197: COUNT hook REFUSED @0x%X (too hot for live play; use Mode191)",
        kMode197TargetRva);
  }
  g_mode197HookOk.store(false);
  return true;  // still "ok" so dual arms; no hook
}

void Mode197OnEndScene() {
  if (!g_mode197HookOk.load())
    return;
  const uint32_t es = ++g_mode197Es;
  const LONG frame = InterlockedExchange(&g_asi_mode197_frame, 0);
  const LONG total = g_asi_mode197_entries;
  if (es <= 8 || (es % 90) == 0)
    Log("Mode197: COUNT es#%u entriesThisFrame=%ld total=%ld target=0x%X hook=COUNT-ONLY "
        "dual=Mode191",
        es, frame, total, kMode197TargetRva);
}

// Mode 199: COUNT-only on Mode198 RARE parent fn-start 0x4D8BF0 (thiscall-56).
constexpr uint32_t kMode199TargetRva = 0x4D8BF0;
void* g_mode199Target = nullptr;
std::atomic<uint32_t> g_mode199Es{0};
std::atomic<bool> g_mode199HookOk{false};
std::atomic<bool> g_mode199TooHot{false};

bool InstallMode199CountHook() {
  if (g_mode199HookOk.load() || g_mode199TooHot.load())
    return g_mode199HookOk.load();
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t addr = base + kMode199TargetRva;
  const auto* b = reinterpret_cast<const uint8_t*>(addr);
  // Expect classic thiscall-56: 56 8B F1
  if (b[0] != 0x56 || b[1] != 0x8B || b[2] != 0xF1) {
    Log("Mode199: FAIL unexpected prologue @0x%X bytes=%02X %02X %02X — wrote stereo=191",
        kMode199TargetRva, b[0], b[1], b[2]);
    WriteStereoModeFile(191);
    return false;
  }
  if (Mode41ForbiddenRva(kMode199TargetRva) || Mode34ForbiddenRva(kMode199TargetRva)) {
    Log("Mode199: FAIL target 0x%X forbidden — wrote stereo=191", kMode199TargetRva);
    WriteStereoModeFile(191);
    return false;
  }
  g_asi_mode199_entries = 0;
  g_asi_mode199_frame = 0;
  g_asi_mode199_tramp = nullptr;
  if (MH_CreateHook(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(&AsiHookMode199Count),
                    &g_asi_mode199_tramp) != MH_OK ||
      MH_EnableHook(reinterpret_cast<void*>(addr)) != MH_OK) {
    Log("Mode199: MH_CreateHook FAIL @0x%X — wrote stereo=191", kMode199TargetRva);
    WriteStereoModeFile(191);
    return false;
  }
  g_mode199Target = reinterpret_cast<void*>(addr);
  g_mode199HookOk.store(true);
  g_mode199Es.store(0);
  Log("Mode199: COUNT naked hook OK @0x%X prologue=568BF1 thiscall-56 tramp=%p "
      "(Mode198 RARE parent; soft avg>8/ES → 191)",
      kMode199TargetRva, g_asi_mode199_tramp);
  return true;
}

void Mode199DisableTooHot(const char* why) {
  if (g_mode199TooHot.exchange(true))
    return;
  if (g_mode199Target) {
    MH_DisableHook(g_mode199Target);
    MH_RemoveHook(g_mode199Target);
    g_mode199Target = nullptr;
  }
  g_mode199HookOk.store(false);
  Log("Mode199: TOO HOT — %s — disabled COUNT hook; wrote stereo=191", why);
  WriteStereoModeFile(191);
}

void Mode199OnEndScene() {
  if (!g_mode199HookOk.load())
    return;
  const uint32_t es = ++g_mode199Es;
  const LONG frame = InterlockedExchange(&g_asi_mode199_frame, 0);
  const LONG total = g_asi_mode199_entries;
  if (es <= 8 || (es % 90) == 0)
    Log("Mode199: COUNT es#%u entriesThisFrame=%ld total=%ld target=0x%X "
        "want~1/ES dual=Mode191",
        es, frame, total, kMode199TargetRva);
  // Soft gate: after warm-up, refuse hot walkers (Mode197 lesson).
  if (es == 90) {
    const float avg = total / 90.f;
    if (avg > 8.f) {
      char buf[96];
      sprintf_s(buf, "avg=%.1f entries/ES over first 90 (cap 8)", avg);
      Mode199DisableTooHot(buf);
    } else {
      Log("Mode199: warm OK avg=%.2f entries/ES over 90 — keep COUNT", avg);
    }
  }
}

// Mode 200: HookDrawWalk dual. 203=@0x4D8BF0; 209=OUTER @0x4D8E80 (calls 203).
constexpr uint32_t kMode200TargetRva = 0x4D8BF0;
constexpr uint32_t kMode204TargetRva = 0x4DE020;  // Mode198 ret 0x4DE0DC → thiscall-56
constexpr uint32_t kMode205TargetRva = 0x4DDD50;  // REJECT math helper
constexpr uint32_t kMode206TargetRva = 0x309D0;   // Mode198 ret 0x30C97 owner shell
constexpr uint32_t kMode207TargetRva = 0x541DC0;  // Mode198 ret 0x5421BB tiny wrapper
constexpr uint32_t kMode208TargetRva = 0x4DA2BA;  // Mode198 ret mid E9 jmp
constexpr uint32_t kMode209TargetRva = 0x4D8E80;  // outer caller of 0x4D8BF0
uint32_t ParentDualTargetRva() {
  if (IsOursFpSameTickParentDual203Outer(GetStereoMode()))
    return kMode209TargetRva;
  if (IsOursFpSameTickParentDualTry6(GetStereoMode()))
    return kMode208TargetRva;
  if (IsOursFpSameTickParentDualTry5(GetStereoMode()))
    return kMode207TargetRva;
  if (IsOursFpSameTickParentDualTry4(GetStereoMode()))
    return kMode206TargetRva;
  if (IsOursFpSameTickParentDualTry3(GetStereoMode()))
    return kMode205TargetRva;
  if (IsOursFpSameTickParentDualTry2(GetStereoMode()))
    return kMode204TargetRva;
  return kMode200TargetRva;
}
int ParentDualKillStereo() {
  if (IsOursFpDeferredHeadBone(GetStereoMode()))
    return 204;
  if (IsOursFpSameTickParentDual203CloseIpdToeIn(GetStereoMode()))
    return 214;
  if (IsOursFpSameTickParentDual203CloseIpdQtr(GetStereoMode()))
    return 213;
  if (IsOursFpSameTickParentDual203CloseIpdHalf(GetStereoMode()))
    return 212;
  if (IsOursFpSameTickParentDual203CloseIpd(GetStereoMode()) ||
      IsOursFpSameTickParentDual203ToeIn(GetStereoMode()) ||
      IsOursFpSameTickParentDual203Outer(GetStereoMode()))
    return 203;
  if (IsOursFpSameTickParentDualTry6(GetStereoMode()) ||
      IsOursFpSameTickParentDualTry5(GetStereoMode()) ||
      IsOursFpSameTickParentDualTry4(GetStereoMode()) ||
      IsOursFpSameTickParentDualTry3(GetStereoMode()))
    return 204;
  if (IsOursFpSameTickParentDualTry2(GetStereoMode()))
    return 203;
  if (IsOursFpSameTickParentDualPose(GetStereoMode()) ||
      IsOursFpSameTickParentDualFreeze(GetStereoMode()) ||
      IsOursFpSameTickParentDualVs(GetStereoMode()))
    return 200;
  return 191;
}
bool ParentDualPrologueOk(const uint8_t* b) {
  // Mode 208: mid-site is E9 rel32 (5 bytes) — MinHook-capable, not a fn prologue.
  if (IsOursFpSameTickParentDualTry6(GetStereoMode()))
    return b[0] == 0xE9;
  return b[0] == 0x56 && b[1] == 0x8B && b[2] == 0xF1;
}
std::atomic<uint32_t> g_mode200Es{0};
std::atomic<uint32_t> g_mode200DualN{0};
std::atomic<bool> g_mode200HookOk{false};
std::atomic<bool> g_mode200TooHot{false};
std::atomic<LONG> g_mode200EntrySum{0};
bool InstallMode200ParentDualHook();
void Mode200OnEndScene();
void Mode200Kill(const char* why);
bool RunMode200ParentDualGuarded(void* self, void* edx);
bool RunMode202ParentDualVsGuarded(void* self, void* edx);

// Mode 198: at most ONE VsRet stack sample per EndScene — find rare parents above
// hot leaf 0x220D0 without VirtualQuery storms (Mode196 1FPS lesson).
constexpr uint32_t kMode198HotLeafRva = 0x220D0;
struct Mode198AggNode {
  uint32_t retRva;
  uint32_t frames;
  uint32_t hits;
};
Mode198AggNode g_mode198Agg[64]{};
int g_mode198AggN = 0;
uint32_t g_mode198FrameRva[48]{};
int g_mode198FrameN = 0;
std::atomic<bool> g_mode198ArmSample{false};
std::atomic<uint32_t> g_mode198Es{0};
std::atomic<uint32_t> g_mode198Samples{0};

bool Mode198IsKnownHotOrForbidden(uint32_t rva) {
  return rva == 0x2C73E || rva == kMode198HotLeafRva || rva == 0x30D13 ||
         rva == 0x309D0 || rva == 0x22187 || rva == 0x2C6A0 || rva == 0x2C6AC ||
         rva == 0x37BD0 || rva == 0x37C01 || rva == 0x1BF010 || rva == 0x4DDAD0 ||
         Mode41ForbiddenRva(rva);
}

void Mode198ResetProbe() {
  g_mode198AggN = 0;
  g_mode198FrameN = 0;
  g_mode198Es.store(0);
  g_mode198Samples.store(0);
  g_mode198ArmSample.store(true);
}

void Mode198OnEndScene() {
  // Merge this EndScene's single sample into aggregates.
  for (int i = 0; i < g_mode198FrameN; ++i) {
    const uint32_t rva = g_mode198FrameRva[i];
    bool found = false;
    for (int j = 0; j < g_mode198AggN; ++j) {
      if (g_mode198Agg[j].retRva == rva) {
        ++g_mode198Agg[j].frames;
        ++g_mode198Agg[j].hits;
        found = true;
        break;
      }
    }
    if (!found && g_mode198AggN < 64)
      g_mode198Agg[g_mode198AggN++] = {rva, 1, 1};
  }
  g_mode198FrameN = 0;
  const uint32_t es = ++g_mode198Es;
  g_mode198ArmSample.store(true);  // allow next ES one sample

  if (es <= 8 || (es % 90) == 0) {
    Log("Mode198: parent-probe epoch=%u samples=%u nodes=%d (1 stack/ES; no VQ) "
        "hotLeaf=0x%X SameFrame=0",
        es, g_mode198Samples.load(), g_mode198AggN, kMode198HotLeafRva);
    int order[64]{};
    const int n = g_mode198AggN;
    for (int i = 0; i < n; ++i)
      order[i] = i;
    for (int i = 0; i < n; ++i) {
      for (int j = i + 1; j < n; ++j) {
        const float ai = g_mode198Agg[order[i]].frames
                             ? (float)g_mode198Agg[order[i]].hits / g_mode198Agg[order[i]].frames
                             : 0.f;
        const float aj = g_mode198Agg[order[j]].frames
                             ? (float)g_mode198Agg[order[j]].hits / g_mode198Agg[order[j]].frames
                             : 0.f;
        // Prefer rarer avg hits/frame, then more frames seen.
        if (aj < ai || (aj == ai && g_mode198Agg[order[j]].frames > g_mode198Agg[order[i]].frames)) {
          const int t = order[i];
          order[i] = order[j];
          order[j] = t;
        }
      }
    }
    int logged = 0;
    for (int i = 0; i < n && logged < 12; ++i) {
      const Mode198AggNode& node = g_mode198Agg[order[i]];
      const float avg =
          node.frames ? (float)node.hits / (float)node.frames : 0.f;
      const bool hot = Mode198IsKnownHotOrForbidden(node.retRva);
      const bool rare = !hot && avg >= 0.5f && avg <= 4.0f && node.frames >= 3;
      if (!rare && !(!hot && logged < 4))
        continue;
      Log("Mode198: CAND %s ret=0x%X frames=%u hits=%u avg/ES=%.2f hook=NO%s",
          rare ? "RARE" : "other", node.retRva, node.frames, node.hits, avg,
          hot ? " HOT/FORBIDDEN" : "");
      ++logged;
    }
  }
}

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
  // Video settings / device recreate: never leave CTimer timestep stuck at 0.
  EndDualTimeFreeze();
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
  g_holdPoseLValid = g_holdPoseRValid = false;
  g_submitPoseLValid = g_submitPoseRValid = false;
}

bool EnsureEyeRts(IDirect3DDevice9* dev, uint32_t w, uint32_t h) {
  if (!dev || w < 16 || h < 16)
    return false;
  const bool mode44 = UsesRtLockFovGate(GetStereoMode());
  const bool haveRts =
      g_texL && g_texR && g_holdL && g_holdR && g_device == dev && g_rtW > 0 && g_rtH > 0;
  if (mode44 && haveRts) {
    // Keep all four default-pool eye textures through harmless FOV publish
    // noise. A lost/replaced device still falls through to allocate again.
    if (w != g_rtW || h != g_rtH)
      ++g_mode44RtSuppressed;
    const uint32_t checks = ++g_mode44RtChecks;
    if ((checks % 600) == 0)
      Log("Mode44: RT lock %ux%u checks=%u recreates=%u suppressed=%u", g_rtW, g_rtH, checks,
          g_mode44RtRecreates.load(), g_mode44RtSuppressed.load());
    return true;
  }
  // Hysteresis: tiny FOV/publish flaps must not recreate 4 RTs (stutter/jump).
  if (haveRts) {
    const int dw = static_cast<int>(w) - static_cast<int>(g_rtW);
    const int dh = static_cast<int>(h) - static_cast<int>(g_rtH);
    if (dw > -48 && dw < 48 && dh > -48 && dh < 48)
      return true;
  }
  if (g_texL && g_texR && g_holdL && g_holdR && g_device == dev && g_rtW == w && g_rtH == h)
    return true;
  const bool replacingRts = g_texL || g_texR || g_holdL || g_holdR;
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
  if (mode44) {
    if (replacingRts)
      ++g_mode44RtRecreates;
    Log("Mode44: RT lock %ux%u initialized recreates=%u", w, h, g_mode44RtRecreates.load());
  }
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
    g_submitPoseL = g_holdPoseL;
    g_submitPoseR = g_holdPoseR;
    g_submitPoseLValid = g_holdPoseLValid;
    g_submitPoseRValid = g_holdPoseRValid;
    const uint32_t n = ++g_pairPromoteCount;
    if (n <= 4 || (n % 300) == 0)
      Log("StereoPairHold: promoted pair #%u (both eyes update together) poseL=%d poseR=%d", n,
          g_submitPoseLValid ? 1 : 0, g_submitPoseRValid ? 1 : 0);
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
  // Mode 168 ONLY: flush before StretchRect. Mode169 drops Flush — 168+Flush
  // caused VK_ERROR_DEVICE_LOST under everyN=1 dual load (2026-07-28).
  if (IsOursFpFlashStable(GetStereoMode())) {
    ID3D9VkInteropDevice* interop = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D9VkInteropDevice),
                                      reinterpret_cast<void**>(&interop))) &&
        interop) {
      interop->FlushRenderingCommands();
      interop->Release();
    }
  }
  const HRESULT hr = dev->StretchRect(bb, nullptr, dst, nullptr, D3DTEXF_NONE);
  dst->Release();
  bb->Release();
  return SUCCEEDED(hr);
}

// Mode 163+: skip eye capture only for TINY side-passes (env/cube ≤512).
// Log 2026-07-28: we also matched half-BB (1272x1260 vs 2544x2521) and quarter
// (636x630) — those are fog/SSR/LOD-style scene buffers, NOT safe to skip.
// Skipping them held last L/R while driving → flash/stutter; capturing mid-pass
// showed missing distant/foggy buildings. Only skip clearly-small env maps.
bool ShouldSkipFlashCapture(IDirect3DDevice9* dev) {
  if (!dev)
    return false;
  IDirect3DSurface9* rt = nullptr;
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt)
    return false;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
    rt->Release();
    return false;
  }
  const bool isBb = (bb == rt);
  D3DSURFACE_DESC rd{}, bd{};
  const bool gotR = SUCCEEDED(rt->GetDesc(&rd));
  const bool gotB = SUCCEEDED(bb->GetDesc(&bd));
  bb->Release();
  rt->Release();
  if (isBb)
    return false;
  if (!gotR || !gotB || bd.Width < 16 || bd.Height < 16)
    return false;
  const uint32_t maxDim = (rd.Width > rd.Height) ? rd.Width : rd.Height;
  // 512 and below: classic env/cube. 636/1024/1272 are scene half-res — allow.
  if (maxDim <= 512) {
    static uint32_t s_skip = 0;
    const uint32_t n = ++s_skip;
    if (n <= 12 || (n % 400) == 0)
      Log("FlashGate: skip tiny env RT %ux%u vs BB %ux%u n=%u", rd.Width, rd.Height,
          bd.Width, bd.Height, n);
    return true;
  }
  return false;
}

bool CopyBbToEyeCanvasGated(IDirect3DDevice9* dev, IDirect3DTexture9* tex, vr::EVREye eye);

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
// Cap keeps two ARGB8 RTs (+hold pair = 4) small enough for 32-bit + FPS.
constexpr uint32_t kCanvasMaxDim = 2048;
// Mode 37 comfort: tighter cap for StretchRect/Submit cost after true-FOV fill.
constexpr uint32_t kCanvasMaxDimComfort = 1536;

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
  const bool comfort = UsesFovComfortPath(GetStereoMode());
  // Live maxDim (F5 on Mode 156+). Comfort default was 1536; non-comfort 2048.
  uint32_t maxDim = GetCanvasMaxDim();
  if (!IsHeadHideNativeOurs(GetStereoMode()) && !IsCleanDualLookMove(GetStereoMode()))
    maxDim = comfort ? kCanvasMaxDimComfort : kCanvasMaxDim;
  if (w > maxDim)
    w = maxDim;
  if (h > maxDim)
    h = maxDim;

  // Mode 37: lock canvas size after first true-FOV sample. FOV wobble was
  // recreating 4 RTs (1536↔1440) every few frames = jump + FPS hit.
  // F5 bumps GetCanvasMaxDimGeneration() to unlock and recreate at new size.
  static uint32_t s_lockW = 0, s_lockH = 0;
  static int s_lockMode = -1;
  static uint32_t s_lockDimGen = 0;
  const uint32_t dimGen = GetCanvasMaxDimGeneration();
  if (!comfort || GetStereoMode() != static_cast<StereoMode>(s_lockMode) ||
      dimGen != s_lockDimGen) {
    s_lockW = s_lockH = 0;
    s_lockMode = static_cast<int>(GetStereoMode());
    s_lockDimGen = dimGen;
  }
  if (comfort && IsGameFovFromCCamActive()) {
    if (s_lockW == 0) {
      // Prefer square-ish comfort canvas once: maxDim × min(bbH,maxDim) so
      // border room survives soft fovadd without per-frame resize.
      s_lockW = maxDim;
      s_lockH = (bbH < maxDim) ? bbH : maxDim;
      if (h > s_lockH)
        s_lockH = (h < maxDim) ? h : maxDim;
      Log("StereoCanvasSize: LOCKED comfort canvas=%ux%u (maxDim=%u; no RT flap)", s_lockW,
          s_lockH, maxDim);
    }
    w = s_lockW;
    h = s_lockH;
  }

  *outW = w;
  *outH = h;

  static uint32_t s_loggedGen = 0xFFFFFFFFu;
  const uint32_t gen = GetGameFovPublishGeneration();
  if (gen != s_loggedGen && gen > 0) {
    s_loggedGen = gen;
    const float fillH = (coverH > 0.05f) ? (gameH / coverH) : 0.f;
    const float fillV = (coverV > 0.05f) ? (gameV / coverV) : 0.f;
    Log("StereoCanvasSize: bb=%ux%u canvas=%ux%u sx=%.2f sy=%.2f gameTan=(%.3f,%.3f) "
        "coverTan=(%.3f,%.3f) fill≈%.0f%%h/%.0f%%v trueFov=%d gen=%u maxDim=%u locked=%d",
        bbW, bbH, w, h, sx, sy, gameH, gameV, coverH, coverV, fillH * 100.f, fillV * 100.f,
        IsGameFovFromCCamActive() ? 1 : 0, gen, maxDim, (comfort && s_lockW) ? 1 : 0);
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
  if (!GetLatchedGameFovTangents(&gameH, &gameV))
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

  // Mode 172/173: pan published view UP by cropping source top.
  if (IsOursFpUiLift(GetStereoMode())) {
    const float liftFrac = IsOursFpSameLPhone(GetStereoMode()) ? 0.16f : 0.10f;
    const LONG cropTop = static_cast<LONG>(SH * liftFrac + 0.5f);
    src.top += cropTop;
    clampRect(&src, static_cast<LONG>(sd.Width), static_cast<LONG>(sd.Height));
  }

  // Mode 183: soft BB inset from right+bottom → phone moves up+left (Mode175 idea,
  // without Submit TextureBounds on the eye-canvas — that warped).
  if (IsOursFpPhoneCanvasInset(GetStereoMode())) {
    constexpr float kCropR = 0.12f;
    constexpr float kCropB = 0.18f;
    const LONG cutR = static_cast<LONG>(SW * kCropR + 0.5f);
    const LONG cutB = static_cast<LONG>(SH * kCropB + 0.5f);
    src.right -= cutR;
    src.bottom -= cutB;
    clampRect(&src, static_cast<LONG>(sd.Width), static_cast<LONG>(sd.Height));
    static bool s_once = false;
    if (!s_once) {
      s_once = true;
      Log("Mode183: canvas BB inset cropR=%.0f%% cropB=%.0f%% (phone up+left; dual kept)",
          kCropR * 100.f, kCropB * 100.f);
    }
  }

  bool ok = false;
  if (rc.right - rc.left >= 16 && rc.bottom - rc.top >= 16 && src.right - src.left >= 16 &&
      src.bottom - src.top >= 16) {
    // Mode 179+: skip ColorFill flash; cover crop StretchRect still clears via overwrite.
    if (!IsOursFpNoColorFill(GetStereoMode()))
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
  // Mode 168 ONLY: flush before BB read. Mode169 keeps no-Flush (see DEVICE_LOST).
  if (IsOursFpFlashStable(GetStereoMode())) {
    ID3D9VkInteropDevice* interop = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D9VkInteropDevice),
                                      reinterpret_cast<void**>(&interop))) &&
        interop) {
      interop->FlushRenderingCommands();
      interop->Release();
    }
  }
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    return false;
  const bool ok = CopySurfToEyeCanvas(dev, bb, tex, eye);
  bb->Release();
  return ok;
}

bool CopyBbToEyeCanvasGated(IDirect3DDevice9* dev, IDirect3DTexture9* tex, vr::EVREye eye) {
  // Mode 168/169: never skip — CopyBbToEyeCanvas reads the backbuffer, not RT0.
  // Tiny-RT skip was designed for current-RT capture and caused HOLDs/flashes
  // whenever DrawScene exited on an env pass (common near water).
  if (IsOursFpAlwaysBbCapture(GetStereoMode()))
    return CopyBbToEyeCanvas(dev, tex, eye);
  if (IsOursFpFlashGate(GetStereoMode()) && ShouldSkipFlashCapture(dev))
    return false;
  return CopyBbToEyeCanvas(dev, tex, eye);
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
long long CompareEyeCanvases(IDirect3DDevice9* dev, bool logResult = true);

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

// Mode 191/192: same-tick dual (no timer freeze).
bool HavePhasePtrs();
void RunPhaseTriplet(void* drawSelf, void* edx);

// Mode 191: BuildRootA×2. Mode 192: PhaseTriplet Right. Mode 194: DrawScene Right.
bool RunSameTickBuildDualGuarded(void* self, void* edx) {
  __try {
    const StereoMode sm = GetStereoMode();
    g_execViewStage = 1;
    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    g_execViewStage = 2;
    g_origRoot[1](self, edx);
    g_execViewStage = 3;
    const bool okL = CopyBbToEyeCanvasGated(g_device, g_texL, vr::Eye_Left);
    g_execViewStage = 4;
    SetStereoEye(StereoEye::Right);
    RefreshLiveCamForStereoEye();
    if (g_device)
      PushLiveCamToD3D(g_device);
    g_execViewStage = 5;
    // 191: full Right BuildRootA (fuses). 192/194: lighter Right — distinct L/R
    // possible but no reliable fusion yet (headset: 194 has angle shift per eye).
    if (IsOursFpSameTickDrawRight(sm) && g_origBuild && g_lastThisDraw) {
      static bool s_once = false;
      if (!s_once) {
        s_once = true;
        Log("Mode194: Right via DrawScene+PushLiveCam (Left was full BuildRootA)");
      }
      g_origBuild(g_lastThisDraw, edx);
    } else if (IsOursFpSameTickPhaseRight(sm) && HavePhasePtrs() && g_lastThisDraw) {
      static bool s_once = false;
      if (!s_once) {
        s_once = true;
        Log("Mode192: Right via PhaseTriplet (Left was full BuildRootA)");
      }
      RunPhaseTriplet(g_lastThisDraw, edx);
    } else {
      static bool s_once = false;
      if (!s_once) {
        s_once = true;
        Log("Mode%d: Right via full BuildRootA (fusion path)", static_cast<int>(sm));
      }
      g_origRoot[1](self, edx);
    }
    g_execViewStage = 6;
    const bool okR = CopyBbToEyeCanvasGated(g_device, g_texR, vr::Eye_Right);
    g_execViewStage = 7;
    if (IsOursFpAtomicEyePair(GetStereoMode())) {
      if (okL && okR)
        g_haveL = g_haveR = true;
    } else {
      if (okL)
        g_haveL = true;
      if (okR)
        g_haveR = true;
    }
    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    SetStereoEye(StereoEye::Left);
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

  // Mode 191/192: real same-tick family — BuildRootA owns L; 191 also R via BuildRootA,
  // 192 R via PhaseTriplet. No CodePause.
  // Mode 200: parent dual owns L/R — BuildRootA stays single (one world build).
  if (IsOursFpSameTickBuildDual(mode) && !IsOursFpSameTickParentDual(mode) &&
      !g_rootDualDead.load() && !g_inDual.load() && IsCamMatrixOverrideEnabled() && g_device) {
    static bool s_armed = false;
    if (!s_armed) {
      s_armed = true;
      Log("Mode%d: BuildRootA same-tick dual armed (phaseRight=%d; no timer freeze)",
          static_cast<int>(mode), IsOursFpSameTickPhaseRight(mode) ? 1 : 0);
    }
    // Soft-wait until EndScene has created eye RTs (first frames).
    if (!g_texL || !g_texR) {
      IDirect3DSurface9* bb = nullptr;
      if (SUCCEEDED(g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC desc{};
        bb->GetDesc(&desc);
        bb->Release();
        uint32_t rtW = desc.Width, rtH = desc.Height;
        ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
        EnsureEyeRts(g_device, rtW, rtH);
      }
    }
    if (!g_texL || !g_texR) {
      g_origRoot[1](self, edx);
      return;
    }
    g_inDual.store(true);
    const bool ok = RunSameTickBuildDualGuarded(self, edx);
    g_inDual.store(false);
    if (!ok) {
      g_rootDualDead.store(true);
      g_haveL = g_haveR = false;
      const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
      const int kill = IsOursFpSameTickDrawRight(mode)      ? 191
                       : IsOursFpSameTickPhaseRight(mode)   ? 191
                       : IsOursFpSameTickOwnerObserve(mode) ? 191
                       : IsOursFpSameTickOwnerCount(mode)   ? 191
                       : IsOursFpSameTickParentProbe(mode)  ? 191
                       : IsOursFpSameTickParentCount(mode)  ? 191
                       : IsOursFpDeferredHeadBone(mode) ? 204
                       : IsOursFpSameTickParentDual203CloseIpdToeIn(mode) ? 214
                       : IsOursFpSameTickParentDual203CloseIpdQtr(mode) ? 213
                       : IsOursFpSameTickParentDual203CloseIpdHalf(mode) ? 212
                       : IsOursFpSameTickParentDual203CloseIpd(mode) ? 203
                       : IsOursFpSameTickParentDual203ToeInStrong(mode) ? 203
                       : IsOursFpSameTickParentDual203ToeIn(mode) ? 203
                       : IsOursFpSameTickParentDual203Outer(mode) ? 203
                       : IsOursFpSameTickParentDualTry6(mode)   ? 204
                       : IsOursFpSameTickParentDualTry5(mode)   ? 204
                       : IsOursFpSameTickParentDualTry4(mode)   ? 204
                       : IsOursFpSameTickParentDualTry3(mode)   ? 204
                       : IsOursFpSameTickParentDualTry2(mode)   ? 203
                       : IsOursFpSameTickParentDualFreeze(mode) ? 200
                       : IsOursFpSameTickParentDualVs(mode)     ? 200
                       : IsOursFpSameTickParentDualPose(mode)   ? 200
                       : IsOursFpSameTickParentDual(mode)   ? 191
                                                            : 187;
      Log("Mode%d: EXCEPTION stage=%ld code=0x%08X addr=%p (exeRva 0x%X) — "
          "PERMANENT disable; wrote stereo=%d",
          static_cast<int>(mode), g_execViewStage, g_execViewExcCode,
          reinterpret_cast<void*>(g_execViewExcAddr),
          static_cast<unsigned>(g_execViewExcAddr - base), kill);
      WriteStereoModeFile(kill);
      SetStereoEye(StereoEye::Left);
      RefreshLiveCamForStereoEye();
      g_origRoot[1](self, edx);
      return;
    }
    const uint32_t n = ++g_rootDualCount;
    if (n <= 6 || (n % 300) == 0)
      Log("Mode%d: same-tick dual #%u haveL=%d haveR=%d sep=%.0fcm", static_cast<int>(mode),
          n, g_haveL ? 1 : 0, g_haveR ? 1 : 0, GetStereoSepMeters() * 100.f);
    if (g_haveL && g_haveR && (n == 5 || (n % 300) == 0))
      CompareEyeCanvases(g_device);
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

  // Mode 200/201: dual Mode199 RARE parent @0x4D8BF0 (single BuildRootA owns world once).
  // Mode 201: full pre-IPD cam freeze for the L/R pair (fusion / look-ghost).
  if (IsOursFpSameTickParentDual(GetStereoMode())) {
    if (g_drawWalkDead.load() || g_mode200TooHot.load()) {
      CallDrawWalkOnceGuarded(self, edx);
      return;
    }
    if (!g_device || !IsCamMatrixOverrideEnabled()) {
      CallDrawWalkOnceGuarded(self, edx);
      return;
    }
    if (!g_texL || !g_texR) {
      IDirect3DSurface9* bb = nullptr;
      if (SUCCEEDED(g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC desc{};
        bb->GetDesc(&desc);
        bb->Release();
        uint32_t rtW = desc.Width, rtH = desc.Height;
        ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
        EnsureEyeRts(g_device, rtW, rtH);
      }
    }
    if (!g_texL || !g_texR || g_inDrawWalkDual.load()) {
      CallDrawWalkOnceGuarded(self, edx);
      return;
    }
    // Mode216: sample HEAD once here (draw shell alive) BEFORE dual flag — never
    // mid-CopyMat, never EndScene-during-load (that crashed after load screen).
    if (IsOursFpDeferredHeadBone(GetStereoMode()) && StereoParentDualReadyForBone())
      SampleDeferredHeadBoneEye();
    const bool freeze = IsOursFpSameTickParentDualFreeze(GetStereoMode());
    const bool vsDual = IsOursFpSameTickParentDualVs(GetStereoMode());
    const bool poseLatch = IsOursFpSameTickParentDualPose(GetStereoMode());
    if (freeze)
      BeginStereoDualCamFreeze();
    if (poseLatch)
      BeginDualHmdPoseLatch();
    g_inDrawWalkDual.store(true);
    // 202 = VS-translate (REJECT). 200/201/203 = full CCam±IPD L/R.
    const bool ok =
        vsDual ? RunMode202ParentDualVsGuarded(self, edx) : RunMode200ParentDualGuarded(self, edx);
    g_inDrawWalkDual.store(false);
    if (poseLatch)
      EndDualHmdPoseLatch();
    if (freeze)
      EndStereoDualCamFreeze();
    if (!ok) {
      const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
      char buf[160];
      sprintf_s(buf, "SEH dual stage=%ld code=0x%08X exeRva=0x%X", g_execViewStage,
                g_execViewExcCode, static_cast<unsigned>(g_execViewExcAddr - base));
      Mode200Kill(buf);
      CallDrawWalkOnceGuarded(self, edx);
      return;
    }
    const uint32_t n = ++g_mode200DualN;
    if (n <= 6 || (n % 300) == 0)
      Log("Mode%d: parent dual #%u haveL=%d haveR=%d sep=%.0fcm freeze=%d poseLatch=%d "
          "vsPatch=%u vsCallsR=%u target=0x%X",
          static_cast<int>(GetStereoMode()), n, g_haveL ? 1 : 0, g_haveR ? 1 : 0,
          GetStereoSepMeters() * 100.f, freeze ? 1 : 0, poseLatch ? 1 : 0,
          StereoVsTranslateCount(), StereoVsRightPassCalls(), ParentDualTargetRva());
    if (g_haveL && g_haveR && (n == 5 || (n % 300) == 0))
      CompareEyeCanvases(g_device);
    return;
  }

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

bool RunMode200ParentDualGuarded(void* self, void* edx) {
  __try {
    g_execViewStage = 1;
    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    if (g_device)
      PushLiveCamToD3D(g_device);
    g_execViewStage = 2;
    g_origDrawWalk(self, edx);
    g_execViewStage = 3;
    const bool okL = CopyBbToEyeCanvasGated(g_device, g_texL, vr::Eye_Left);
    g_execViewStage = 4;
    SetStereoEye(StereoEye::Right);
    RefreshLiveCamForStereoEye();
    if (g_device)
      PushLiveCamToD3D(g_device);
    g_execViewStage = 5;
    g_origDrawWalk(self, edx);
    g_execViewStage = 6;
    const bool okR = CopyBbToEyeCanvasGated(g_device, g_texR, vr::Eye_Right);
    g_execViewStage = 7;
    if (IsOursFpAtomicEyePair(GetStereoMode())) {
      if (okL && okR)
        g_haveL = g_haveR = true;
    } else {
      if (okL)
        g_haveL = true;
      if (okR)
        g_haveR = true;
    }
    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    SetStereoEye(StereoEye::Left);
    return false;
  }
}

// Mode 202: CCam stays Left both walks; Right enables SetVSConstF view-translate.
bool RunMode202ParentDualVsGuarded(void* self, void* edx) {
  __try {
    g_execViewStage = 1;
    float dx = 0.f, dy = 0.f, dz = 0.f;
    float cx = 0.f, cy = 0.f, cz = 0.f;
    g_vsPatchOn.store(false);
    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    if (g_device)
      PushLiveCamToD3D(g_device);
    g_execViewStage = 2;
    g_origDrawWalk(self, edx);
    g_execViewStage = 3;
    const bool okL = CopyBbToEyeCanvasGated(g_device, g_texL, vr::Eye_Left);
    const bool sane = GetStereoEyeRightDeltaWorld(&dx, &dy, &dz) &&
                      GetLastStereoCamPos(&cx, &cy, &cz);
    if (!sane) {
      if (okL)
        g_haveL = true;
      SetStereoEye(StereoEye::Left);
      RefreshLiveCamForStereoEye();
      return true;
    }
    g_vsPatchCam[0] = cx;
    g_vsPatchCam[1] = cy;
    g_vsPatchCam[2] = cz;
    g_vsPatchDelta[0] = dx;
    g_vsPatchDelta[1] = dy;
    g_vsPatchDelta[2] = dz;
    // CCam stays Left — VS translate only (no CCam+VS stack / double IPD).
    g_vsPatchOn.store(true);
    g_execViewStage = 5;
    g_origDrawWalk(self, edx);
    g_vsPatchOn.store(false);
    g_execViewStage = 6;
    const bool okR = CopyBbToEyeCanvasGated(g_device, g_texR, vr::Eye_Right);
    g_execViewStage = 7;
    if (IsOursFpAtomicEyePair(GetStereoMode())) {
      if (okL && okR)
        g_haveL = g_haveR = true;
    } else {
      if (okL)
        g_haveL = true;
      if (okR)
        g_haveR = true;
    }
    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    g_vsPatchOn.store(false);
    SetStereoEye(StereoEye::Left);
    return false;
  }
}

bool InstallMode200ParentDualHook() {
  if (g_mode200HookOk.load() || g_mode200TooHot.load())
    return g_mode200HookOk.load();
  const uint32_t targetRva = ParentDualTargetRva();
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t addr = base + targetRva;
  const auto* b = reinterpret_cast<const uint8_t*>(addr);
  const int kill = ParentDualKillStereo();
  if (!ParentDualPrologueOk(b)) {
    Log("Mode%d: FAIL unexpected prologue @0x%X bytes=%02X %02X %02X — wrote stereo=%d",
        static_cast<int>(GetStereoMode()), targetRva, b[0], b[1], b[2], kill);
    WriteStereoModeFile(kill);
    return false;
  }
  if (Mode41ForbiddenRva(targetRva) || Mode34ForbiddenRva(targetRva)) {
    Log("Mode%d: FAIL target 0x%X forbidden — wrote stereo=%d",
        static_cast<int>(GetStereoMode()), targetRva, kill);
    WriteStereoModeFile(kill);
    return false;
  }
  g_drawWalkDead.store(false);
  g_drawWalkEntries.store(0);
  g_mode200Es.store(0);
  g_mode200DualN.store(0);
  g_mode200EntrySum.store(0);
  if (!InstallDrawWalkAt(addr)) {
    Log("Mode%d: InstallDrawWalkAt FAIL @0x%X — wrote stereo=%d",
        static_cast<int>(GetStereoMode()), targetRva, kill);
    WriteStereoModeFile(kill);
    return false;
  }
  g_mode200HookOk.store(true);
  Log("Mode%d: PARENT DUAL DrawWalk OK @0x%X prologue=568BF1 thiscall-56 "
      "(single BuildRootA; poseLatch=%d; soft avg>8/ES → kill %d)",
      static_cast<int>(GetStereoMode()), targetRva,
      IsOursFpSameTickParentDualPose(GetStereoMode()) ? 1 : 0, kill);
  return true;
}

void Mode200Kill(const char* why) {
  if (g_mode200TooHot.exchange(true))
    return;
  EndStereoDualCamFreeze();
  EndDualHmdPoseLatch();
  g_drawWalkDead.store(true);
  if (g_drawWalkAddr) {
    MH_DisableHook(g_drawWalkAddr);
    MH_RemoveHook(g_drawWalkAddr);
    g_origDrawWalk = nullptr;
    g_drawWalkAddr = nullptr;
  }
  g_mode200HookOk.store(false);
  SetStereoEye(StereoEye::Left);
  RefreshLiveCamForStereoEye();
  const int kill = ParentDualKillStereo();
  Log("Mode%d: KILL — %s — wrote stereo=%d", static_cast<int>(GetStereoMode()), why, kill);
  WriteStereoModeFile(kill);
}

void Mode200OnEndScene() {
  if (!g_mode200HookOk.load())
    return;
  const uint32_t es = ++g_mode200Es;
  const LONG frame = static_cast<LONG>(g_drawWalkEntries.exchange(0));
  g_mode200EntrySum.fetch_add(frame);
  const LONG total = g_mode200EntrySum.load();
  if (es <= 8 || (es % 90) == 0)
    Log("Mode%d: dual-walk es#%u entriesThisFrame=%ld total=%ld dualN=%u target=0x%X "
        "want~1/ES (single BuildRootA)",
        static_cast<int>(GetStereoMode()), es, frame, total, g_mode200DualN.load(),
        ParentDualTargetRva());
  if (es == 90) {
    const float avg = total / 90.f;
    if (avg > 8.f) {
      char buf[96];
      sprintf_s(buf, "avg=%.1f entries/ES over first 90 (cap 8)", avg);
      Mode200Kill(buf);
    } else {
      Log("Mode%d: warm OK avg=%.2f entries/ES over 90 — keep parent dual",
          static_cast<int>(GetStereoMode()), avg);
    }
  }
  // Mode 206 lesson: warm can be cold, then spike to 35+/ES (car freeze / FPS death).
  if (es > 90 && frame > 8 &&
      (IsOursFpSameTickParentDualTry4(GetStereoMode()) ||
       IsOursFpSameTickParentDualTry5(GetStereoMode()) ||
       IsOursFpSameTickParentDualTry6(GetStereoMode()) ||
       IsOursFpSameTickParentDual203Outer(GetStereoMode()))) {
    char buf[96];
    sprintf_s(buf, "entriesThisFrame=%ld > 8 after warm (206 HOT lesson)", frame);
    Mode200Kill(buf);
  }
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
long long CompareEyeCanvases(IDirect3DDevice9* dev, bool logResult) {
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
  if (logResult)
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

bool Mode41ForbiddenRva(uint32_t rva) {
  return rva == 0x2C6AC || rva == 0x37BD0 || rva == 0x1BF010 || rva == 0x4DDAD0;
}

const char* Mode41CallBeforeReturn(uintptr_t base, uint32_t retRva, uint32_t* outCallRva,
                                   uint32_t* outTargetRva, uint8_t* outModrm,
                                   uint8_t* outLength);

bool Mode196SafeReadPtr(uintptr_t addr, uintptr_t* out) {
  if (!out)
    return false;
  __try {
    *out = *reinterpret_cast<const uintptr_t*>(addr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool Mode196DecodeOwnerDisp(uintptr_t base, uint32_t* outDisp, uint32_t* outCallRva,
                            uint8_t* outModrm, uint8_t* outLen) {
  uint32_t callRva = 0, targetRva = 0;
  uint8_t modrm = 0, callLen = 0;
  const char* call =
      Mode41CallBeforeReturn(base, 0x30D13, &callRva, &targetRva, &modrm, &callLen);
  if (std::strcmp(call, "FF/2") != 0 || callLen < 6)
    return false;
  const auto* p = reinterpret_cast<const uint8_t*>(base + callRva);
  if (p[0] != 0xFF)
    return false;
  const uint8_t mod = static_cast<uint8_t>(p[1] >> 6);
  const uint8_t rm = static_cast<uint8_t>(p[1] & 7);
  // Mode42: FF 90 disp32 = call [eax+disp32] (mod=2, /2, rm=eax).
  if (mod != 2 || rm != 0)
    return false;
  if (outDisp)
    *outDisp = *reinterpret_cast<const uint32_t*>(p + 2);
  if (outCallRva)
    *outCallRva = callRva;
  if (outModrm)
    *outModrm = modrm;
  if (outLen)
    *outLen = callLen;
  return true;
}

void Mode196NoteOwnerFromStack(uintptr_t* sp, int nSlots, uintptr_t base) {
  if (!sp || nSlots < 2)
    return;
  bool haveEdge = false;
  for (int i = 1; i < nSlots; ++i) {
    if (sp[i] == base + 0x30D13) {
      haveEdge = true;
      break;
    }
  }
  if (!haveEdge)
    return;
  ++g_mode196EdgeHits;

  uint32_t disp = 0, callRva = 0;
  uint8_t modrm = 0, callLen = 0;
  if (!Mode196DecodeOwnerDisp(base, &disp, &callRva, &modrm, &callLen))
    return;
  g_mode196HaveDisp = true;
  g_mode196Disp = disp;
  g_mode196CallRva = callRva;
  g_mode196Modrm = modrm;
  g_mode196CallLen = callLen;

  for (int i = 1; i < nSlots; ++i) {
    const uintptr_t obj = sp[i];
    if (obj < 0x10000 || (obj & 3u) != 0)
      continue;
    // Skip code/return addresses inside the exe image.
    if (obj >= base + 0x1000 && obj <= base + 0xC00000)
      continue;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(obj), &mbi, sizeof(mbi)))
      continue;
    if (mbi.State != MEM_COMMIT)
      continue;
    const DWORD prot = mbi.Protect & 0xFFu;
    if (prot != PAGE_READONLY && prot != PAGE_READWRITE && prot != PAGE_WRITECOPY &&
        prot != PAGE_EXECUTE_READ && prot != PAGE_EXECUTE_READWRITE &&
        prot != PAGE_EXECUTE_WRITECOPY)
      continue;

    uintptr_t vtable = 0;
    if (!Mode196SafeReadPtr(obj, &vtable))
      continue;
    if (vtable < base + 0x1000 || vtable > base + 0xC00000)
      continue;
    uintptr_t target = 0;
    if (!Mode196SafeReadPtr(vtable + disp, &target))
      continue;
    if (target < base + 0x1000 || target > base + 0xC00000)
      continue;
    MEMORY_BASIC_INFORMATION mbiT{};
    if (!VirtualQuery(reinterpret_cast<const void*>(target), &mbiT, sizeof(mbiT)))
      continue;
    const DWORD tprot = mbiT.Protect & 0xFFu;
    if (tprot != PAGE_EXECUTE && tprot != PAGE_EXECUTE_READ &&
        tprot != PAGE_EXECUTE_READWRITE && tprot != PAGE_EXECUTE_WRITECOPY)
      continue;
    const uint32_t targetRva = static_cast<uint32_t>(target - base);
    if (Mode41ForbiddenRva(targetRva))
      continue;

    bool found = false;
    for (int j = 0; j < g_mode196OwnerN; ++j) {
      if (g_mode196Owners[j].obj == obj && g_mode196Owners[j].targetRva == targetRva) {
        ++g_mode196Owners[j].hits;
        found = true;
        break;
      }
    }
    if (!found && g_mode196OwnerN < 16) {
      g_mode196Owners[g_mode196OwnerN++] = {obj, vtable, targetRva, 1};
    }
  }
}

void Mode41NoteFrameNode(uint32_t retRva, uint8_t slot) {
  if (retRva == kMode34VsRetRva || Mode41ForbiddenRva(retRva))
    return;
  for (int i = 0; i < g_mode41FrameN; ++i) {
    if (g_mode41Frame[i].retRva == retRva && g_mode41Frame[i].slot == slot) {
      ++g_mode41Frame[i].hits;
      return;
    }
  }
  if (g_mode41FrameN < 96)
    g_mode41Frame[g_mode41FrameN++] = {retRva, slot, 1};
}

const char* Mode41CallBeforeReturn(uintptr_t base, uint32_t retRva, uint32_t* outCallRva,
                                   uint32_t* outTargetRva, uint8_t* outModrm,
                                   uint8_t* outLength) {
  if (outCallRva)
    *outCallRva = 0;
  if (outTargetRva)
    *outTargetRva = 0;
  if (outModrm)
    *outModrm = 0;
  if (outLength)
    *outLength = 0;
  const uintptr_t ret = base + retRva;
  if (ret < base + 6)
    return "none";
  const auto* b = reinterpret_cast<const uint8_t*>(ret);
  // Direct relative call: its five-byte instruction ends exactly at the saved
  // return. This is stronger evidence than merely resolving an FPO stack word.
  if (b[-5] == 0xE8) {
    const int32_t rel = *reinterpret_cast<const int32_t*>(b - 4);
    if (outCallRva)
      *outCallRva = retRva - 5;
    if (outTargetRva)
      *outTargetRva = static_cast<uint32_t>(ret + static_cast<intptr_t>(rel) - base);
    return "E8";
  }
  // Indirect call sites are still useful chain nodes, but need a later, safe
  // object/vtable trace before they can become a replay seam.
  if (b[-2] == 0xFF)
    return "FF";
  if (b[-6] == 0xFF && b[-5] == 0x15) {
    if (outCallRva)
      *outCallRva = retRva - 6;
    return "FF15";
  }
  // x86's FF /2 covers vtable and register-indirect calls too. Mode 41 only
  // recognized FF 15, so common forms such as `FF 90 disp32` appeared to have
  // no caller at all. Decode the instruction length only; never dereference the
  // operand or alter execution. This is the missing evidence for 0x30D13.
  for (uint8_t len = 2; len <= 7; ++len) {
    const uint8_t* p = b - len;
    if (p[0] != 0xFF || ((p[1] >> 3) & 7) != 2)
      continue;
    const uint8_t mod = p[1] >> 6;
    const uint8_t rm = p[1] & 7;
    uint8_t expected = 2;
    if (rm == 4) {
      const uint8_t sib = p[2];
      if (mod == 0 && (sib & 7) == 5)
        expected = 6;
      else if (mod == 1)
        expected = 3;
      else if (mod == 2)
        expected = 6;
    } else if (mod == 0 && rm == 5) {
      expected = 6;
    } else if (mod == 1) {
      expected = 3;
    } else if (mod == 2) {
      expected = 6;
    }
    if (mod == 3)
      expected = 2;
    if (expected != len)
      continue;
    if (outCallRva)
      *outCallRva = retRva - len;
    if (outModrm)
      *outModrm = p[1];
    if (outLength)
      *outLength = len;
    return "FF/2";
  }
  return "none";
}

void Mode41FinishFrame() {
  const uint32_t es = ++g_mode41Es;
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  for (int i = 0; i < g_mode41FrameN; ++i) {
    const Mode41FrameNode& node = g_mode41Frame[i];
    for (int j = 0; j < g_mode41AggN; ++j) {
      if (g_mode41Agg[j].retRva == node.retRva && g_mode41Agg[j].slot == node.slot) {
        ++g_mode41Agg[j].frames;
        g_mode41Agg[j].hits += node.hits;
        goto next_node;
      }
    }
    if (g_mode41AggN < 96)
      g_mode41Agg[g_mode41AggN++] = {node.retRva, 1, node.hits, node.slot};
  next_node:;
  }
  g_mode41FrameN = 0;

  if (es > 8 && (es % 90) != 0)
    return;
  const bool mode42 = GetStereoMode() == StereoMode::ReplayOwnerCountProbe;
  const bool mode196 = IsOursFpSameTickOwnerObserve(GetStereoMode());
  const char* label = mode196 ? "Mode196" : mode42 ? "Mode42" : "Mode41";
  Log("%s: replay-chain epoch=%u VsRetHits=%u nodes=%d tid=%u SameFrame=0 distinctEyes=0",
      label, es, g_mode41VsRetHits.load(), g_mode41AggN, g_mode41VsTid);
  if (mode196) {
    Log("Mode196: OWNER-EDGE cadence edgeHits=%u owners=%d haveDisp=%d disp=0x%X "
        "call@0x%X modrm=0x%02X len=%u hook=NO replay=NO",
        g_mode196EdgeHits.load(), g_mode196OwnerN, g_mode196HaveDisp ? 1 : 0, g_mode196Disp,
        g_mode196CallRva, g_mode196Modrm, g_mode196CallLen);
    int ord[16]{};
    const int oc = g_mode196OwnerN;
    for (int i = 0; i < oc; ++i)
      ord[i] = i;
    for (int i = 0; i < oc; ++i) {
      for (int j = i + 1; j < oc; ++j) {
        if (g_mode196Owners[ord[j]].hits > g_mode196Owners[ord[i]].hits) {
          const int t = ord[i];
          ord[i] = ord[j];
          ord[j] = t;
        }
      }
    }
    const int lim = (std::min)(oc, 8);
    for (int i = 0; i < lim; ++i) {
      const Mode196OwnerHit& o = g_mode196Owners[ord[i]];
      Log("Mode196: OWNER-CAND #%d eaxObj=%p vtable=%p targetRva=0x%X hits=%u "
          "hook=NO replay=NO",
          i, reinterpret_cast<void*>(o.obj), reinterpret_cast<void*>(o.vtable), o.targetRva,
          o.hits);
    }
  }
  int order[96]{};
  int count = g_mode41AggN;
  for (int i = 0; i < count; ++i)
    order[i] = i;
  for (int i = 0; i < count; ++i) {
    for (int j = i + 1; j < count; ++j) {
      const Mode41AggNode& a = g_mode41Agg[order[i]];
      const Mode41AggNode& b = g_mode41Agg[order[j]];
      if (b.frames > a.frames || (b.frames == a.frames && b.hits > a.hits)) {
        const int t = order[i];
        order[i] = order[j];
        order[j] = t;
      }
    }
  }
  const int limit = (std::min)(count, 12);
  for (int i = 0; i < limit; ++i) {
    const Mode41AggNode& node = g_mode41Agg[order[i]];
    const uintptr_t ret = base + node.retRva;
    uintptr_t fn = FindThiscallNear(ret, 0x800);
    if (!fn)
      fn = FindFnStartNear(ret);
    const uint32_t fnRva = fn ? static_cast<uint32_t>(fn - base) : 0;
    const char* abi = "no-start";
    if (fn)
      (void)Mode32ClassifyPrologue(fn, &abi);
    uint32_t callRva = 0, targetRva = 0;
    uint8_t modrm = 0, callLen = 0;
    const char* call =
        Mode41CallBeforeReturn(base, node.retRva, &callRva, &targetRva, &modrm, &callLen);
    Log("%s: CHAIN slot=%u ret=0x%X fn=0x%X abi=%s call=%s@0x%X->0x%X "
        "modrm=0x%02X len=%u frames=%u hits=%u hook=NO",
        label, node.slot, node.retRva, fnRva, abi, call, callRva, targetRva, modrm, callLen,
        node.frames, node.hits);
    if ((mode42 || mode196) && node.retRva == 0x30D13) {
      Log("%s: OWNER-EDGE ret=0x30D13 enclosing=0x%X abi=%s call=%s@0x%X "
          "modrm=0x%02X len=%u cadenceFrames=%u hits=%u hook=NO replay=NO",
          label, fnRva, abi, call, callRva, modrm, callLen, node.frames, node.hits);
    }
  }
}

void Mode41OnEndScene(IDirect3DDevice9* device) {
  TemporalCapturePairHold(device);  // Same safe behavior as Mode 40.
  Mode41FinishFrame();
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
  // Mode 30/35..43 pair-hold: passthrough native exec; EndScene does temporal.
  if (mode == StereoMode::PhaseDualDeviceVs || mode == StereoMode::FovRecomputeSite ||
      mode == StereoMode::FovRecomputeTrueCanvas || mode == StereoMode::FovCanvasComfort ||
      mode == StereoMode::AerPoseSubmit || mode == StereoMode::FovCanvasLowMotion ||
      mode == StereoMode::FovCanvasMotionGuard || mode == StereoMode::ReplayCallChainProbe ||
      mode == StereoMode::ReplayOwnerCountProbe ||
      mode == StereoMode::FovCanvasMotionGuardFast ||
      mode == StereoMode::FovCanvasMotionGuardRtLock ||
      mode == StereoMode::HeadOwnedCamSpike ||
      mode == StereoMode::HeadOwnedCamFullPose ||
      mode == StereoMode::HeadOwnedCamLeveledPitchFlip ||
      mode == StereoMode::HeadOwnedCamPitchStable ||
      mode == StereoMode::HeadOwnedCamPedCoupled ||
      mode == StereoMode::HeadOwnedCamStereoAlways ||
      mode == StereoMode::HeadOwnedCamStereoAer ||
      mode == StereoMode::HeadOwnedCamStereoSwap ||
      mode == StereoMode::HeadOwnedCamStereoSoftGuard) {
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

  // Mode 38: Luke Ross AER — stamp capture-time HMD pose so SteamVR can
  // reproject the stale eye (OpenVR #1253). Mode 136: late-latch — sample pose
  // immediately before Submit (not capture-time). Fall back to Submit_Default
  // if pose missing.
  vr::EVRCompositorError err = vr::VRCompositorError_None;
  const StereoMode curMode = GetStereoMode();
  const bool lateLatch = IsCleanDualLateLatch(curMode);
  const bool wantPose = UsesAerPoseSubmit(curMode) || lateLatch;
  vr::HmdMatrix34_t latePose{};
  bool poseOk = false;
  if (lateLatch)
    poseOk = SampleLateLatchHmdPose(&latePose);
  else if (wantPose)
    poseOk = (eye == vr::Eye_Left) ? g_submitPoseLValid : g_submitPoseRValid;
  if (wantPose && poseOk) {
    vr::VRTextureWithPose_t tp{};
    tp.handle = &vd;
    tp.eType = vr::TextureType_Vulkan;
    tp.eColorSpace = vr::ColorSpace_Gamma;
    tp.mDeviceToAbsoluteTracking =
        lateLatch ? latePose
                  : ((eye == vr::Eye_Left) ? g_submitPoseL : g_submitPoseR);
    const vr::VRTextureBounds_t* bounds = nullptr;
    vr::VRTextureBounds_t phoneLift{};
    if (IsOursFpPhoneBounds(curMode)) {
      phoneLift.uMin = 0.14f;
      phoneLift.uMax = 1.f;
      phoneLift.vMin = 0.42f;
      phoneLift.vMax = 1.f;
      bounds = &phoneLift;
    }
    err = vr::VRCompositor()->Submit(eye, &tp, bounds, vr::Submit_TextureWithPose);
    static uint32_t s_aer = 0;
    if ((++s_aer) <= 8 || (s_aer % 300) == 0) {
      if (lateLatch)
        Log("LateLatch: eye=%s TextureWithPose=1 err=%d",
            eye == vr::Eye_Left ? "L" : "R", static_cast<int>(err));
      else
        Log("AERPose: eye=%s submitWithPose=1 err=%d", eye == vr::Eye_Left ? "L" : "R",
            static_cast<int>(err));
    }
    if (err != vr::VRCompositorError_None) {
      // Runtime may ignore pose (historic WMR bug) — still try default once.
      err = vr::VRCompositor()->Submit(eye, &t, nullptr, vr::Submit_Default);
      if (s_aer <= 8 || (s_aer % 300) == 0)
        Log("%s: eye=%s pose-submit failed → Default err=%d",
            lateLatch ? "LateLatch" : "AERPose", eye == vr::Eye_Left ? "L" : "R",
            static_cast<int>(err));
    }
  } else {
    if (wantPose) {
      static uint32_t s_miss = 0;
      if ((++s_miss) <= 4 || (s_miss % 300) == 0)
        Log("%s: eye=%s pose missing → Submit_Default",
            lateLatch ? "LateLatch" : "AERPose", eye == vr::Eye_Left ? "L" : "R");
    }
    // Fusion baseline: nullptr bounds + temporal IPD only.
    // Mode 175/176: keep phone framing (same UV both eyes).
    const vr::VRTextureBounds_t* bounds = nullptr;
    vr::VRTextureBounds_t phoneLift{};
    if (IsOursFpPhoneBounds(curMode)) {
      phoneLift.uMin = 0.14f;
      phoneLift.uMax = 1.f;
      phoneLift.vMin = 0.42f;
      phoneLift.vMax = 1.f;
      bounds = &phoneLift;
    }
    err = vr::VRCompositor()->Submit(eye, &t, bounds, vr::Submit_Default);
  }
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

// Mode 120: synced sequential DrawScene×2 only (Praydog analogue). Never BuildRootA×2.
// PhaseA/C already ran once; we rebuild world draw list L then R with IPD.
bool RunMode120DrawSceneDualGuarded(void* drawSelf, void* edx) {
  if (!g_origBuild || !drawSelf || !g_device || !g_texL || !g_texR)
    return false;
  const bool fpHost = IsExternalFpHost(GetStereoMode());
  const bool atomicPair = IsOursFpAtomicEyePair(GetStereoMode());
  const bool pairFreeze = IsOursFpDualCamFreeze(GetStereoMode());
  const bool monoPair = IsOursFpMonoPair(GetStereoMode());
  // Mode189: freeze sim for the WHOLE L+R pair (not only between eyes like Mode184).
  // Mode184 (legacy): step-only between L and R — poke disabled; leave path for A/B.
  const bool sameTick = IsOursFpSameTickDual(GetStereoMode());
  const bool timeFreezeMid = IsOursFpTimeFreeze(GetStereoMode());
  __try {
    if (pairFreeze)
      BeginStereoDualCamFreeze();
    if (sameTick)
      BeginDualTimeFreeze();

    if (monoPair) {
      // ONE center draw (IPD off in ApplyHmdToCam); same BB → L+R with SAME eye crop.
      SetStereoEye(StereoEye::Left);
      RefreshLiveCamForStereoEye();
      if (fpHost)
        PushLiveCamToD3D(g_device);
      g_origBuild(drawSelf, edx);
      const bool okL = CopyBbToEyeCanvasGated(g_device, g_texL, vr::Eye_Left);
      // Force Left eye projection for Right texture too → pixel-identical pair.
      const bool okR = CopyBbToEyeCanvasGated(g_device, g_texR, vr::Eye_Left);
      if (sameTick)
        EndDualTimeFreeze();
      if (pairFreeze)
        EndStereoDualCamFreeze();
      if (atomicPair) {
        if (okL && okR)
          g_haveL = g_haveR = true;
      } else {
        if (okL)
          g_haveL = true;
        if (okR)
          g_haveR = true;
      }
      SetStereoEye(StereoEye::Left);
      RefreshLiveCamForStereoEye();
      if (fpHost)
        PushLiveCamToD3D(g_device);
      return true;
    }

    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    if (fpHost)
      PushLiveCamToD3D(g_device);
    g_origBuild(drawSelf, edx);
    // Mode 176: plain full StretchRect (no ColorFill / cover-canvas) — flicker A/B.
    const bool okL = IsOursFpStereoSimple(GetStereoMode())
                         ? CopyBbToEye(g_device, g_texL)
                         : CopyBbToEyeCanvasGated(g_device, g_texL, vr::Eye_Left);

    if (timeFreezeMid)
      BeginDualTimeFreeze();
    if (sameTick)
      RepinDualTimeMs();

    SetStereoEye(StereoEye::Right);
    RefreshLiveCamForStereoEye();
    if (fpHost)
      PushLiveCamToD3D(g_device);
    // Mode 185: fuller same-tick Right = PhaseA→PhaseC→DrawScene (not bare DrawScene×2).
    if (IsOursFpPhaseRightDual(GetStereoMode())) {
      static bool s_once = false;
      if (!s_once) {
        s_once = true;
        Log("Mode185: Right via PhaseTriplet havePhase=%d sameTick=%d",
            HavePhasePtrs() ? 1 : 0, sameTick ? 1 : 0);
      }
      RunPhaseTriplet(drawSelf, edx);
    } else {
      g_origBuild(drawSelf, edx);
    }
    const bool okR = IsOursFpStereoSimple(GetStereoMode())
                         ? CopyBbToEye(g_device, g_texR)
                         : CopyBbToEyeCanvasGated(g_device, g_texR, vr::Eye_Right);

    if (timeFreezeMid || sameTick)
      EndDualTimeFreeze();

    if (pairFreeze)
      EndStereoDualCamFreeze();

    // Mode 168/169: only publish a fresh pair when BOTH eyes captured this tick.
    // Partial update (old L + new R) looks like a full-screen flash.
    if (atomicPair) {
      if (okL && okR)
        g_haveL = g_haveR = true;
      else {
        static uint32_t s_partial = 0;
        const uint32_t n = ++s_partial;
        if (n <= 8 || (n % 200) == 0)
          Log("Mode%d: dual capture partial okL=%d okR=%d — kept previous pair n=%u",
              static_cast<int>(GetStereoMode()), okL ? 1 : 0, okR ? 1 : 0, n);
      }
    } else {
      if (okL)
        g_haveL = true;
      if (okR)
        g_haveR = true;
    }

    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    if (fpHost)
      PushLiveCamToD3D(g_device);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    EndDualTimeFreeze();
    EndStereoDualCamFreeze();
    return false;
  }
}

void __fastcall HookBuildRenderList(void* self, void* edx) {
  if (!g_origBuild)
    return;

  g_lastThisDraw = self;
  const StereoMode mode = GetStereoMode();
  if (mode == StereoMode::RenderRootProbe)
    LogRenderRootRet("BuildDrawScene", _ReturnAddress());

  // Mode 120–136: DrawScene×2 every N + HOLD off-ticks (hitchcut). No FPS HUD.
  // 120=LKG dualn=2; 121=dualn=3; 122=dualn=4+look/pose HOLD; 123=fill; 124=zoom;
  // 125=122+123; 126=123 fill + POSEHOLD + live BB; 127=126 + tighter POSEHOLD/dualn3;
  // 128=127 fill/tight but HOLD last stereo on pose (no look-rate LIVELOOK);
  // 129=look→force same-tick dual; calm dualn=3; pose→mono BB ≥200ms hyst;
  // 130=look→identical BB both eyes ≥350ms hyst; calm dualn=3 HOLD;
  // 131=130 + pitch-stable + 600ms + BB/3 + calm POSEHOLD (FAILED HMD starve);
  // 132=131 pitch-stable but BB→both EVERY EndScene; calm dualn=3 HOLD;
  // 133=FAILED same-tex L+R Submit (jump/fusion gone);
  // 134=132-safe twin + dualn=3 HOLD (FAILED HMD freeze via stale HOLD);
  // 135=ALWAYS-FRESH: everyN=1 dual; look/pose→BB×2; never bare HOLD.
  // Mode 140: same DrawScene dual; IPD-only on FirstPerson cam.
  if (UsesDrawSceneDualPath(mode)) {
    // Mode 191: BuildRootA owns L/R dual — DrawScene is a single native draw per walk.
    if (IsOursFpSameTickBuildDual(mode)) {
      g_origBuild(self, edx);
      PerfDebugNoteDrawScene(false, g_haveL && g_haveR, 0);
      return;
    }
    // Mode 174/175: single stock DrawScene only — no dual, no eye-canvas (Mode0 BB Submit).
    if (IsOursFpBbPhone(mode)) {
      g_origBuild(self, edx);
      PerfDebugNoteDrawScene(false, false, 0);
      return;
    }
    // Mode 177 AER: ONE eye per DrawScene (alternate). Other eye stays 1 frame stale.
    if (IsOursFpAer(mode)) {
      if (!IsStereoRenderArmed() || !g_device) {
        g_origBuild(self, edx);
        return;
      }
      IDirect3DSurface9* bb = nullptr;
      if (FAILED(g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
        g_origBuild(self, edx);
        return;
      }
      D3DSURFACE_DESC desc{};
      bb->GetDesc(&desc);
      bb->Release();
      uint32_t rtW = desc.Width, rtH = desc.Height;
      ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
      EnsureEyeRts(g_device, rtW, rtH);
      if (!g_texL || !g_texR) {
        g_origBuild(self, edx);
        return;
      }
      static bool s_right = false;
      s_right = !s_right;
      SetStereoEye(s_right ? StereoEye::Right : StereoEye::Left);
      RefreshLiveCamForStereoEye();
      g_origBuild(self, edx);
      SnapshotHoldPose(s_right);
      if (s_right) {
        if (CopyBbToEye(g_device, g_texR)) {
          g_haveR = true;
          g_submitPoseR = g_holdPoseR;
          g_submitPoseRValid = g_holdPoseRValid;
        }
      } else {
        if (CopyBbToEye(g_device, g_texL)) {
          g_haveL = true;
          g_submitPoseL = g_holdPoseL;
          g_submitPoseLValid = g_holdPoseLValid;
        }
      }
      SetStereoEye(StereoEye::Left);
      RefreshLiveCamForStereoEye();
      static uint32_t s_aerN = 0;
      const uint32_t n = ++s_aerN;
      if (n <= 8 || (n % 300) == 0)
        Log("Mode177: AER #%u eye=%s haveL=%d haveR=%d poseL=%d poseR=%d", n,
            s_right ? "R" : "L", g_haveL ? 1 : 0, g_haveR ? 1 : 0,
            g_submitPoseLValid ? 1 : 0, g_submitPoseRValid ? 1 : 0);
      PerfDebugNoteDrawScene(true, g_haveL && g_haveR, 0);
      return;
    }
    // Mode193 door/interior: DrawScene itself AVs — skip calling it; HOLD last stereo.
    if (IsOursFpHeadBoneCam(mode)) {
      const DWORD skipUntil = g_mode193SkipDrawUntilMs.load();
      if (skipUntil != 0 && GetTickCount() < skipUntil) {
        if (g_haveL && g_haveR)
          StereoDualMarkHold();
        else
          StereoDualMarkLiveLook();
        return;
      }
    }
    if (g_mode120DualDead.load() || g_inDual.load() || !IsStereoRenderArmed() ||
        !g_device || !g_texL || !g_texR) {
      g_origBuild(self, edx);
      return;
    }
    static DWORD s_prev = 0;
    static uint32_t s_gate = 0;
    static uint32_t s_tick = 0;
    const DWORD now = GetTickCount();
    const DWORD gap = (s_prev != 0) ? (now - s_prev) : 0;
    s_prev = now;
    if (gap >= StereoDualHitchMs()) {
      g_origBuild(self, edx);
      // Mode 126/127/135/168 hitch: live BB. Mode 169: HOLD last stereo (no LIVELOOK flash).
      // Mode 128–134: keep last L/R.
      if (IsCleanDualAlwaysFresh(mode) || IsCleanDualFpsLiveLook(mode) ||
          IsOursFpFlashStable(mode))
        StereoDualMarkLiveLook();
      else if (g_haveL && g_haveR)
        StereoDualMarkHold();
      PerfDebugNoteDrawScene(false, g_haveL && g_haveR, gap);
      return;
    }
    // Mode 135: look (low) OR pose → fresh BB both eyes (never bare HOLD).
    if (StereoDualShouldAlwaysFreshMono()) {
      g_origBuild(self, edx);
      StereoDualMarkLiveLook();
      PerfDebugNoteDrawScene(false, false, gap);
      return;
    }
    // Mode 131/132/133/134: sustained look → identical BB both eyes ≥600ms; pitch thresh > yaw.
    if (StereoDualShouldMonoLookPitchHyst()) {
      g_origBuild(self, edx);
      StereoDualMarkLiveLook();
      PerfDebugNoteDrawScene(false, false, gap);
      return;
    }
    // Mode 130: look/pose → identical BB both eyes with ≥350ms hysteresis.
    if (StereoDualShouldMonoLookHyst()) {
      g_origBuild(self, edx);
      StereoDualMarkLiveLook();
      PerfDebugNoteDrawScene(false, false, gap);
      return;
    }
    // Mode 129: pose budget → mono BB both eyes with ≥200ms hysteresis (caps cost).
    if (StereoDualShouldPoseMonoHyst()) {
      g_origBuild(self, edx);
      StereoDualMarkLiveLook();
      PerfDebugNoteDrawScene(false, false, gap);
      return;
    }
    // Mode 126/127: look/pose → live BB both eyes (fixes Mode122 snap from stale HOLD).
    if (StereoDualShouldLiveLook()) {
      g_origBuild(self, edx);
      StereoDualMarkLiveLook();
      PerfDebugNoteDrawScene(false, false, gap);
      return;
    }
    // Mode 128 / Mode 131/132/133/134 calm: pose budget → HOLD last stereo (no LIVELOOK mono /
    // no look-rate; look uses monolook above — no mismatched HOLD pair). Mode 135 excluded.
    if (StereoDualShouldPoseHoldStereo()) {
      g_origBuild(self, edx);
      if (g_haveL && g_haveR)
        StereoDualMarkHold();
      PerfDebugNoteDrawScene(false, g_haveL && g_haveR, gap);
      return;
    }
    // Mode 122/125: look-rate / pose-budget stale HOLD before dual cadence.
    if (StereoDualShouldLookHold()) {
      g_origBuild(self, edx);
      if (g_haveL && g_haveR)
        StereoDualMarkHold();
      PerfDebugNoteDrawScene(false, g_haveL && g_haveR, gap);
      return;
    }
    if (s_gate < StereoDualGateNeed()) {
      ++s_gate;
      g_origBuild(self, edx);
      return;
    }
    ++s_tick;
    const uint32_t everyN = StereoDualEveryN();
    // Mode 129 look: force same-tick dual (skip everyN HOLD → no L/R age desync).
    const bool forceLookDual = StereoDualShouldForceLookDual();
    if (!forceLookDual && (s_tick % everyN) != 0u) {
      g_origBuild(self, edx);
      // Mode 135/168/170 safety: never bare HOLD on off-tick (everyN should be 1).
      if (IsCleanDualAlwaysFresh(mode) || IsOursFpFlashStable(mode) || IsOursFpFreshDual(mode))
        StereoDualMarkLiveLook();
      else if (g_haveL && g_haveR)
        StereoDualMarkHold();
      PerfDebugNoteDrawScene(false, g_haveL && g_haveR, gap);
      return;
    }
    g_inDual.store(true);
    const bool ok = RunMode120DrawSceneDualGuarded(self, edx);
    g_inDual.store(false);
    if (!ok) {
      // Mode193: door/interior AVs often surface here (bone thiscall mid-stream).
      // Soft-fail one frame — do NOT permanent-kill dual / write 120.
      if (IsOursFpHeadBoneCam(mode)) {
        static uint32_t s_softN = 0;
        ++s_softN;
        const DWORD pauseMs = 8000;
        g_mode193SkipDrawUntilMs.store(GetTickCount() + pauseMs);
        NotifyHeadBoneSoftSkip(pauseMs, "DrawScene dual AV");
        if (s_softN <= 8 || (s_softN % 30) == 0)
          Log("Mode193: DrawScene dual AV soft #%u — SKIP DrawScene %ums; HOLD last stereo "
              "(mono rebuild also AVs at doors)",
              s_softN, pauseMs);
        // Keep g_haveL/R — submit last good stereo pair. Do NOT call DrawScene.
        if (g_haveL && g_haveR)
          StereoDualMarkHold();
        PerfDebugNoteDrawScene(false, g_haveL && g_haveR, gap);
        return;
      }
      g_mode120DualDead.store(true);
      g_haveL = g_haveR = false;
      // Experiments (121+) fall back to LKG 120; Mode 120 keeps kill=51; Mode140 → 0.
      // Mode193 head-bone: keep Mode187 (WorldScale) — not bare 120 (door AV kill).
      const int killMode = IsExternalFpHost(mode)                 ? 0
                           : (mode == StereoMode::CleanDualLookMove) ? 51
                           : IsOursFpHeadBoneCam(mode)               ? 187
                                                                     : 120;
      Log("Mode%d: EXCEPTION in DrawScene dual — permanently disabled; wrote stereo=%d",
          static_cast<int>(mode), killMode);
      WriteStereoModeFile(killMode);
      PerfDebugVrNote("CleanDual DrawScene dual EXCEPTION — fell back");
      g_origBuild(self, edx);
      return;
    }
    StereoDualMarkHold();
    PerfDebugNoteDrawScene(true, true, gap);
    const uint32_t n = ++g_mode120DualN;
    if (n <= 6 || (n % 300) == 0)
      Log("Mode%d: DRAWSCENE dual #%u haveL=%d haveR=%d sep=%.0fcm everyN=%u "
          "(%s; kill=167/162/120)",
          static_cast<int>(mode), n, g_haveL ? 1 : 0, g_haveR ? 1 : 0,
          GetStereoSepMeters() * 100.f, everyN,
          IsOursFpFreshDual(mode)            ? "FRESH everyN=1"
              : IsOursFpFlashStable(mode)    ? "STABLE everyN=1"
              : IsOursFpSafeFlicker(mode)    ? "SAFE dualn=2 atomic"
                                             : "HOLD off-tick");
    return;
  }

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
// Mode 37/38: latch gameTan on Left so Right uses identical canvas rects.
// Mode 38: also stamp HMD pose at capture for Submit_TextureWithPose.
void TemporalCapturePairHold(IDirect3DDevice9* device) {
  if (!device || !g_holdL || !g_holdR || !g_texL || !g_texR)
    return;
  LogCachedIpdOnce();
  // Mode 48–52: refresh tracked CopyMat matrices immediately before each eye capture
  // so a late follow-cam overwrite cannot stale the backbuffer view.
  if (UsesPreCaptureCamRefresh(GetStereoMode()))
    RefreshLiveCamForStereoEye();
  const StereoEye eye = GetStereoEye();
  const bool latchPair = UsesFovComfortPath(GetStereoMode());
  if (eye == StereoEye::Left) {
    if (latchPair)
      LatchGameFovForPair();
    if (CopyBbToEyeCanvas(device, g_holdL, vr::Eye_Left)) {
      SnapshotHoldPose(false);
      g_pairAwaitingR = true;
    }
    SetStereoEye(StereoEye::Right);
  } else {
    if (CopyBbToEyeCanvas(device, g_holdR, vr::Eye_Right) && g_pairAwaitingR) {
      SnapshotHoldPose(true);
      float rotDeg = 0.f, moveCm = 0.f;
      bool directSubmitGuard = false;
      const StereoMode sm = GetStereoMode();
      if (sm == StereoMode::HeadOwnedCamStereoSoftGuard) {
        if (ComputeHoldPoseDelta(&rotDeg, &moveCm)) {
          const SoftGuardAction action = Mode53SoftGuardAction(rotDeg, moveCm);
          if (action == SoftGuardAction::FullMono) {
            const bool sameFrameL = CopyBbToEyeCanvas(device, g_texL, vr::Eye_Left);
            const bool sameFrameR = CopyBbToEyeCanvas(device, g_texR, vr::Eye_Right);
            if (sameFrameL && sameFrameR) {
              g_haveL = g_haveR = true;
              g_submitPoseL = g_holdPoseR;
              g_submitPoseR = g_holdPoseR;
              g_submitPoseLValid = g_submitPoseRValid = g_holdPoseRValid;
              directSubmitGuard = true;
              const uint32_t hardN = ++g_mode53FullMono;
              if (hardN <= 6 || (hardN % 120) == 0)
                Log("Mode53: HARD-GUARD full mono #%u rot=%.2fdeg move=%.2fcm "
                    "(>=15deg or >=12cm — extreme turn only)",
                    hardN, rotDeg, moveCm);
            } else {
              Log("Mode53: hard-guard copy failed; promoting normal temporal pair");
            }
          } else if (action == SoftGuardAction::AerDistinct) {
            const uint32_t softN = ++g_mode53SoftGuard;
            if (softN <= 6 || (softN % 120) == 0)
              Log("Mode53: SOFT-GUARD AER distinct #%u rot=%.2fdeg move=%.2fcm "
                  "(distinct L/R preserved + Submit_TextureWithPose; no flatten)",
                  softN, rotDeg, moveCm);
          }
        }
      } else if (Mode40NeedsMonoGuard(&rotDeg, &moveCm)) {
        // Both eye canvases now originate from this exact backbuffer epoch.
        // They retain their correct per-eye canvas geometry, but intentionally
        // contain one camera view until the next calm L/R temporal pair.
        const bool fast = GetStereoMode() == StereoMode::FovCanvasMotionGuardFast ||
                          GetStereoMode() == StereoMode::FovCanvasMotionGuardRtLock ||
                          GetStereoMode() == StereoMode::HeadOwnedCamSpike ||
                          GetStereoMode() == StereoMode::HeadOwnedCamFullPose ||
                          GetStereoMode() == StereoMode::HeadOwnedCamLeveledPitchFlip ||
                          GetStereoMode() == StereoMode::HeadOwnedCamPitchStable ||
                          GetStereoMode() == StereoMode::HeadOwnedCamPedCoupled;
        IDirect3DTexture9* dstL = fast ? g_texL : g_holdL;
        IDirect3DTexture9* dstR = fast ? g_texR : g_holdR;
        const bool sameFrameL = CopyBbToEyeCanvas(device, dstL, vr::Eye_Left);
        const bool sameFrameR = CopyBbToEyeCanvas(device, dstR, vr::Eye_Right);
        if (sameFrameL && sameFrameR) {
          g_holdPoseL = g_holdPoseR;
          g_holdPoseLValid = g_holdPoseRValid;
          if (fast) {
            // Mode 43: both submit textures now contain one current R epoch.
            // Skip PromotionHoldPair's two additional full-texture copies.
            g_haveL = g_haveR = true;
            g_submitPoseL = g_holdPoseR;
            g_submitPoseR = g_holdPoseR;
            g_submitPoseLValid = g_submitPoseRValid = g_holdPoseRValid;
            directSubmitGuard = true;
          }
          const uint32_t guarded = ++g_mode40MonoPairs;
          if (guarded <= 6 || (guarded % 120) == 0)
            Log("Mode%d: MOTION-GUARD mono pair #%u SameFrame: L+R from current R frame "
                "rot=%.2fdeg move=%.2fcm directSubmit=%d",
                static_cast<int>(GetStereoMode()), guarded, rotDeg, moveCm,
                directSubmitGuard ? 1 : 0);
        } else {
          Log("Mode40: motion guard copy failed; promoting normal temporal pair");
        }
      }
      if (!directSubmitGuard)
        PromoteHoldPair(device);
      if (UsesStereoAlwaysDistinct(GetStereoMode())) {
        const long long diff = CompareEyeCanvases(device, false);
        if (diff >= 0) {
          const uint32_t pairs = ++g_stereoAuditPairs;
          g_stereoAuditDiffSum.fetch_add(diff);
          if (diff < 200)
            ++g_stereoAuditZero;
          if (pairs <= 6 || (pairs % 120) == 0) {
            const long long sum = g_stereoAuditDiffSum.load();
            const uint32_t zero = g_stereoAuditZero.load();
            if (sm == StereoMode::HeadOwnedCamStereoSoftGuard) {
              Log("Mode53: StereoAudit pairs=%u avgDiff=%lld zeroRate=%.1f%% sep=%.0fcm "
                  "scale=%.2f softGuard=%u hardMono=%u",
                  pairs, sum / static_cast<long long>(pairs),
                  pairs ? (100.f * zero / pairs) : 0.f, GetStereoSepMeters() * 100.f,
                  GetStereoScale(), g_mode53SoftGuard.load(), g_mode53FullMono.load());
            } else {
              Log("Mode50: StereoAudit pairs=%u avgDiff=%lld zeroRate=%.1f%% sep=%.0fcm "
                  "scale=%.2f motionGuard=0",
                  pairs, sum / static_cast<long long>(pairs),
                  pairs ? (100.f * zero / pairs) : 0.f, GetStereoSepMeters() * 100.f,
                  GetStereoScale());
            }
          }
        }
      }
      g_pairAwaitingR = false;
    }
    if (latchPair)
      ClearGameFovPairLatch();
    SetStereoEye(StereoEye::Left);
  }
  RefreshLiveCamForStereoEye();
  const uint32_t n = ++g_temporalFrames;
  if (n <= 6 || (n % 120) == 0)
    Log("StereoPairHold: frame #%u captured=%s awaitingR=%d haveSubmit=%d/%d sep=%.0fcm "
        "latch=%d poseLR=%d/%d",
        n, eye == StereoEye::Left ? "L" : "R", g_pairAwaitingR ? 1 : 0, g_haveL ? 1 : 0,
        g_haveR ? 1 : 0, GetStereoSepMeters() * 100.f, latchPair ? 1 : 0,
        g_holdPoseLValid ? 1 : 0, g_holdPoseRValid ? 1 : 0);
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
  StereoMode mode = GetStereoMode();
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

  if (UsesDrawSceneDualPath(mode)) {
    // Mode 120 LKG / 121–136 experiments / Mode 140 FP-host: DrawScene dual + eye RTs.
    // Mode 140: no FOV site (FirstPerson owns FOV). Kill → 120 / 0.
    SetStereoEye(StereoEye::Left);
    g_haveL = g_haveR = false;
    g_mode30Phase.store(static_cast<int>(Mode30Phase::PairHold));
    g_execDualDead.store(true);
    g_mode120DualDead.store(false);
    g_mode120DualN.store(0);
    g_pairAwaitingR = false;
    g_holdPoseLValid = g_holdPoseRValid = false;
    g_submitPoseLValid = g_submitPoseRValid = false;
    g_vsPatchOn.store(false);
    ClearGameFovPairLatch();
    StereoDualClearHold();
    StereoDualClearLiveLook();
    const bool fovOk =
        IsExternalFpHost(mode) ? false : InstallFovRecomputeSiteHook();
    g_ok = InstallAllThreePhases();
    if (IsOursFpSameTickBuildDual(mode)) {
      g_rootDualDead.store(false);
      g_rootDualCount.store(0);
      // BuildRootA (root[1]) owns L/R dual; ExecRoot passthrough.
      const bool rootOk = InstallRootProbeHooks(2);
      g_ok = g_ok.load() && rootOk;
      if (IsOursFpSameTickOwnerCount(mode)) {
        if (!InstallMode197CountHook())
          g_ok = false;
      }
      if (IsOursFpSameTickParentProbe(mode))
        Mode198ResetProbe();
      if (IsOursFpSameTickParentCount(mode)) {
        if (!InstallMode199CountHook())
          g_ok = false;
      }
      if (IsOursFpSameTickParentDual(mode)) {
        if (!InstallMode200ParentDualHook())
          g_ok = false;
      }
    }
    const int mid = static_cast<int>(mode);
    if (IsExternalFpHost(mode)) {
      Log("StereoRender: mode %d EXTERNAL-FP DUAL (FirstPerson cam/hide/FOV; "
          "DrawScene×2 IPD-only; dualn=2 HOLD; HMD→mouse look; no fovadd site; no AER) "
          "ok=%d",
          mid, g_ok.load() ? 1 : 0);
      Log("StereoRender: kill-switch - 147 soft-move / 150 native-hide+FP / 151 ours+hide");
    } else if (IsHeadHideNativeOurs(mode)) {
      const char* tag = mode == StereoMode::OursFpSameTickParentDual204DeferredHead
                            ? "+SAME-TICK-PARENT-DUAL-204-DEFHEAD"
                        : mode == StereoMode::OursFpSameTickParentDual203CloseIpdToeIn
                            ? "+SAME-TICK-PARENT-DUAL-203CLOSEIPD+TOEIN"
                        : mode == StereoMode::OursFpSameTickParentDual203CloseIpdQtr
                            ? "+SAME-TICK-PARENT-DUAL-203CLOSEIPD-Q"
                        : mode == StereoMode::OursFpSameTickParentDual203CloseIpdHalf
                            ? "+SAME-TICK-PARENT-DUAL-203CLOSEIPD-H"
                        : mode == StereoMode::OursFpSameTickParentDual203CloseIpd
                            ? "+SAME-TICK-PARENT-DUAL-203CLOSEIPD"
                        : mode == StereoMode::OursFpSameTickParentDual203ToeInStrong
                            ? "+SAME-TICK-PARENT-DUAL-203TOEIN-S"
                        : mode == StereoMode::OursFpSameTickParentDual203ToeIn
                            ? "+SAME-TICK-PARENT-DUAL-203TOEIN"
                        : mode == StereoMode::OursFpSameTickParentDual203Outer
                            ? "+SAME-TICK-PARENT-DUAL-203OUTER"
                        : mode == StereoMode::OursFpSameTickParentDualTry6
                            ? "+SAME-TICK-PARENT-DUAL-TRY6"
                        : mode == StereoMode::OursFpSameTickParentDualTry5
                            ? "+SAME-TICK-PARENT-DUAL-TRY5"
                        : mode == StereoMode::OursFpSameTickParentDualTry4
                            ? "+SAME-TICK-PARENT-DUAL-TRY4"
                        : mode == StereoMode::OursFpSameTickParentDualTry3
                            ? "+SAME-TICK-PARENT-DUAL-TRY3"
                        : mode == StereoMode::OursFpSameTickParentDualTry2
                            ? "+SAME-TICK-PARENT-DUAL-TRY2"
                        : mode == StereoMode::OursFpSameTickParentDualPose
                            ? "+SAME-TICK-PARENT-DUAL-POSE"
                        : mode == StereoMode::OursFpSameTickParentDualVs
                            ? "+SAME-TICK-PARENT-DUAL-VS"
                        : mode == StereoMode::OursFpSameTickParentDualFreeze
                            ? "+SAME-TICK-PARENT-DUAL-FZ"
                        : mode == StereoMode::OursFpSameTickParentDual ? "+SAME-TICK-PARENT-DUAL"
                        : mode == StereoMode::OursFpSameTickParentCount ? "+SAME-TICK-PARENT-COUNT"
                        : mode == StereoMode::OursFpSameTickParentProbe ? "+SAME-TICK-PARENT"
                        : mode == StereoMode::OursFpSameTickOwnerCount ? "+SAME-TICK-OWNER-COUNT"
                        : mode == StereoMode::OursFpSameTickOwnerObserve ? "+SAME-TICK-OWNER-OBS"
                        : mode == StereoMode::OursFpSameTickDrawRight ? "+SAME-TICK-DRAW-R"
                        : mode == StereoMode::OursFpHeadBoneThiscall ? "+HEAD-BONE-TC"
                        : mode == StereoMode::OursFpSameTickPhaseRight ? "+SAME-TICK-PHASE-R"
                        : mode == StereoMode::OursFpSameTickBuildDual ? "+SAME-TICK-BUILD"
                        : mode == StereoMode::OursFpHeadBoneReturn ? "+HEAD-BONE-RET"
                        : mode == StereoMode::OursFpSameTickDual ? "+SAME-TICK"
                        : mode == StereoMode::OursFpHeadBoneWorldScale ? "+HEAD-BONE+WS"
                        : mode == StereoMode::OursFpTrueWorldScale ? "+TRUE-WORLDSCALE"
                        : mode == StereoMode::OursFpZoomOut ? "+ZOOM-OUT"
                        : mode == StereoMode::OursFpPhaseRightDual ? "+PHASE-RIGHT-DUAL"
                        : mode == StereoMode::OursFpTimeFreeze ? "+TIME-FREEZE"
                        : mode == StereoMode::OursFpPhoneCanvasInset ? "+PHONE-CANVAS-INSET"
                        : mode == StereoMode::OursFpPhoneStereo       ? "+PHONE-STEREO"
                        : mode == StereoMode::OursFpNoColorFill     ? "+NO-COLORFILL"
                        : mode == StereoMode::OursFpSameFrameFreeze ? "+SAME-FRAME-FREEZE"
                        : IsOursFpAer(mode)             ? "+AER"
                        : IsOursFpStereoSimple(mode)    ? "+STEREO-SIMPLE"
                        : IsOursFpBbPhoneNudge(mode)    ? "+BB-PHONE-NUDGE"
                        : IsOursFpBbPhone(mode)         ? "+BB-MONO+PHONE"
                        : IsOursFpSameLPhone(mode)      ? "+SAME-L+PHONE"
                        : IsOursFpMonoPair(mode)        ? "+MONO-PAIR"
                        : IsOursFpPairFreeze(mode)      ? "+PAIR-FREEZE"
                        : IsOursFpFreshDual(mode)       ? "+FRESH-DUAL"
                        : IsOursFpSafeFlicker(mode)     ? "+SAFE-FLICKER"
                        : IsOursFpStableCarHud(mode)  ? "+STABLE+CAR+HUD"
                        : IsOursFpCarHud(mode)        ? "+CAR+HUD"
                        : IsOursFpHudLayout(mode)       ? "+HUD-LAYOUT"
                        : IsOursFpEnterCarFp(mode)      ? "+ENTER-CAR-FP"
                        : IsOursFpInCarHead(mode)       ? "+IN-CAR-HEAD"
                        : IsOursFpFlashGate(mode) && mode == StereoMode::OursFpFlashGate
                                                       ? "+FLASH-GATE"
                        : IsOursFpSquareWideFov(mode)   ? "+SQUARE-WIDE-FOV"
                        : IsOursFpSquareZeroOverscan(mode) ? "+SQUARE-ZERO-OS"
                        : IsOursFpSquareLowOverscan(mode)  ? "+SQUARE-LOW-OS"
                        : IsOursFpStrongOverscan(mode)   ? "+STRONG-OVERSCAN"
                        : IsOursFpSquareRes(mode)        ? "+SQUARE-RES"
                        : IsOursFpMildOverscan(mode)     ? "+OVERSCAN"
                        : IsOursFpEyeCenterLow(mode)     ? "+EYE-LOW+VRES"
                        : IsOursFpEyeCenterCam(mode)     ? "+EYE-CENTER"
                        : IsOursFpWideAspectFit(mode)    ? "+FP-ASPECT-FIT"
                        : IsOursFpWideUnderPublish(mode) ? "+FP-WIDE"
                        : IsOursFpFovProfile(mode)       ? "+FP-PRESENCE"
                                                         : "";
      const char* detail =
          mode == StereoMode::OursFpSameTickParentDual204DeferredHead
              ? "; Mode204 + deferred HEAD EndScene sample (kill=204)"
          : mode == StereoMode::OursFpSameTickParentDual203CloseIpdToeIn
              ? "; Mode203 + IPD 0.25cm + toe-in ~1.5m (kill=214)"
          : mode == StereoMode::OursFpSameTickParentDual203CloseIpdQtr
              ? "; Mode203 + IPD 0.25cm (kill=213)"
          : mode == StereoMode::OursFpSameTickParentDual203CloseIpdHalf
              ? "; Mode203 + IPD 0.5cm (kill=212)"
          : mode == StereoMode::OursFpSameTickParentDual203CloseIpd
              ? "; Mode203 + IPD 1cm (kill=203)"
          : mode == StereoMode::OursFpSameTickParentDual203ToeInStrong
              ? "; Mode203 + stronger toe-in ~0.85m (kill=203)"
          : mode == StereoMode::OursFpSameTickParentDual203ToeIn
              ? "; Mode203 + toe-in ~1.5m MIXED mild (kill=203)"
          : mode == StereoMode::OursFpSameTickParentDual203Outer
              ? "; REJECT OUTER @0x4D8E80 — use 203"
          : mode == StereoMode::OursFpSameTickParentDualTry6
              ? "; REJECT mid-jmp 0x4DA2BA — use 204"
          : mode == StereoMode::OursFpSameTickParentDualTry5
              ? "; REJECT tiny @0x541DC0 — same-image no fuse; use 204"
          : mode == StereoMode::OursFpSameTickParentDualTry4
              ? "; MIXED HOT owner shell @0x309D0 — prefer 204"
          : mode == StereoMode::OursFpSameTickParentDualTry3
              ? "; REJECT math helper @0x4DDD50 — use 204"
          : mode == StereoMode::OursFpSameTickParentDualTry2
              ? "; Mode203 path on next Mode198 parent @0x4DE020 (kill=203 MILESTONE)"
          : mode == StereoMode::OursFpSameTickParentDualPose
              ? "; MILESTONE Mode200 full CCam±IPD + ONE HMD pose latch @0x4D8BF0"
          : mode == StereoMode::OursFpSameTickParentDualVs
              ? "; REJECT VS-translate — blurs on look; use 203"
          : mode == StereoMode::OursFpSameTickParentDualFreeze
              ? "; REJECT freeze — use 203/200"
          : mode == StereoMode::OursFpSameTickParentDual
              ? "; SINGLE BuildRootA + parent dual @0x4D8BF0 RefreshLiveCam L/R"
          : mode == StereoMode::OursFpSameTickParentCount
              ? "; Mode191 dual + COUNT @0x4D8BF0 (Mode198 RARE thiscall-56)"
          : mode == StereoMode::OursFpSameTickParentProbe
              ? "; Mode191 dual + 1 VsRet stack/ES parent hunt above 0x220D0"
          : mode == StereoMode::OursFpSameTickOwnerCount
              ? "; Mode191 dual + COUNT-ONLY @0x220D0 (stack-arg naked; no VirtualQuery)"
          : mode == StereoMode::OursFpSameTickOwnerObserve
              ? "; DONE — observe disabled (was 1FPS); use 197 or 191"
          : mode == StereoMode::OursFpSameTickDrawRight
              ? "; distinct L/R angles via DrawScene Right, but NO fusion; prefer 191"
          : mode == StereoMode::OursFpHeadBoneThiscall
              ? "; Mode187 + CE CPed GetBonePosition thiscall HEAD (crouch/sprint)"
          : mode == StereoMode::OursFpSameTickPhaseRight
              ? "; PhaseTriplet Right — smooth, no fusion; prefer 191"
          : mode == StereoMode::OursFpSameTickBuildDual
              ? "; MILESTONE BuildRootA x2 L/R CopyMat (no CodePause; audio OK)"
          : mode == StereoMode::OursFpHeadBoneReturn
              ? "; FAILED ScriptHook natives from CopyMat — bone OFF"
          : mode == StereoMode::OursFpSameTickDual
              ? "; FAILED audio hitch — CTimer/CodePause freeze; prefer 191"
          : mode == StereoMode::OursFpHeadBoneWorldScale
              ? "; FAILED under-map out-ptr bone — use 187"
          : mode == StereoMode::OursFpTrueWorldScale
              ? "; Mode186 + scale×IPD (UEVR size; F11 100→200%; uncapped half-sep)"
          : mode == StereoMode::OursFpZoomOut
              ? "; Mode185 PhaseRight dual + fpfov=110 zoom-out (cupboard/near size)"
          : mode == StereoMode::OursFpPhaseRightDual
              ? "; CHECKPOINT Mode170 dual+canvas + Right PhaseA→PhaseC→DrawScene"
          : mode == StereoMode::OursFpTimeFreeze
              ? "; FAILED CTimer freeze — flicker unchanged; poke disabled"
          : mode == StereoMode::OursFpPhoneCanvasInset
              ? "; FAILED soft BB crop — prefer 185 / 170"
          : mode == StereoMode::OursFpPhoneStereo
              ? "; FAILED Submit-UV-on-canvas — prefer 185 / 175"
          : mode == StereoMode::OursFpNoColorFill
              ? "; Mode178 full cam freeze + no canvas ColorFill (keep cover crop)"
          : mode == StereoMode::OursFpSameFrameFreeze
              ? "; Mode170 dual+canvas + freeze FULL pre-IPD cam (basis+pos); only ±IPD"
          : IsOursFpAer(mode)
              ? "; 1 DrawScene/frame alt L/R + TextureWithPose stale eye; phone bounds"
          : IsOursFpStereoSimple(mode)
              ? "; dual+IPD + plain BB StretchRect (no canvas) + phone bounds"
          : IsOursFpBbPhoneNudge(mode)
              ? "; Mode174 BB + bounds uMin=0.14 vMin=0.42 (phone up+left)"
          : IsOursFpBbPhone(mode)
              ? "; Mode0 BB Submit (no dual/canvas) + bounds vMin=0.30 phone lift"
          : IsOursFpSameLPhone(mode)
              ? "; sameL Submit both eyes + UI lift=minimap 16% (phone); stereo flat"
          : IsOursFpMonoPair(mode)
              ? "; 1 DrawScene→both eyes identical + UI lift~10% (phone); stereo flat"
          : IsOursFpPairFreeze(mode)
              ? "; Mode170 + freeze ped eye ORIGIN across L/R (driving flicker)"
          : IsOursFpFreshDual(mode)
              ? "; Mode169 + everyN=1 (no HOLD); still NO Flush; hitch→HOLD"
          : IsOursFpSafeFlicker(mode)
              ? "; Mode167 cam/HUD + dualn=2 atomic (no skip-gate; NO Flush; hitch→HOLD)"
          : IsOursFpStableCarHud(mode)
              ? "; FAILED DEVICE_LOST — prefer 170; everyN=1+Flush killed GPU"
          : IsOursFpCarHud(mode)
              ? "; sit-cam + HUD inset + mission directions"
          : IsOursFpHudLayout(mode)
              ? "; HUD radar/status inward (hudoff)"
          : IsOursFpEnterCarFp(mode)
              ? "; enter-car keep FP + in-car head + flash gate"
          : IsOursFpInCarHead(mode)
              ? "; in-car eye back (vehcamoff) + flash gate"
          : (IsOursFpFlashGate(mode) && mode == StereoMode::OursFpFlashGate)
              ? "; skip small RT side-pass (water/envmap; not strict BB)"
          : IsOursFpSquareWideFov(mode)
              ? "; Luke square + fpfov=100 zoom-out (0% OS)"
          : IsOursFpSquareZeroOverscan(mode)
              ? "; Luke square + overscan=0% exact fit A/B"
          : IsOursFpSquareLowOverscan(mode)
              ? "; Luke square + overscan~2% (less crop)"
          : IsOursFpStrongOverscan(mode)
              ? "; overscan~10% more bar shrink; 158=LKG"
          : IsOursFpSquareRes(mode)
              ? "; 1440sq commandline + mild overscan"
          : IsOursFpMildOverscan(mode)
              ? "; under-publish overscan~6% shrink bars"
          : IsOursFpEyeCenterLow(mode)
              ? "; eyeH=65cm + F5 VR canvas maxDim"
          : IsOursFpEyeCenterCam(mode)
              ? "; ped-fixed eye pivot + aspect-fit FOV"
          : IsOursFpWideAspectFit(mode)
              ? "; CCam=fpfov aspect-fit under-publish"
          : IsOursFpWideUnderPublish(mode)
              ? "; CCam=fpfov under-publish gameTan=COVER"
          : IsOursFpFovProfile(mode) ? "; eyefwd=0+Window0 look-down jacket; F7 live"
                                     : "";
      Log("StereoRender: mode %d OURS+HIDE%s (%s + our HMD cam + "
          "native SET_DRAW%s; FirstPerson.asi OFF) ok=%d fovSite=%d",
          mid, tag,
          IsOursFpDeferredHeadBone(mode)
              ? "single BuildRootA + parent dual 204+deferredHEAD@4DE020"
              : IsOursFpSameTickParentDual203CloseIpdToeIn(mode)
              ? "single BuildRootA + parent dual 203+closeIpd0.25+toeIn@4D8BF0"
              : IsOursFpSameTickParentDual203CloseIpdQtr(mode)
              ? "single BuildRootA + parent dual 203+closeIpd0.25cm@4D8BF0"
              : IsOursFpSameTickParentDual203CloseIpdHalf(mode)
              ? "single BuildRootA + parent dual 203+closeIpd0.5cm@4D8BF0"
              : IsOursFpSameTickParentDual203CloseIpd(mode)
              ? "single BuildRootA + parent dual 203+closeIpd1cm@4D8BF0"
              : IsOursFpSameTickParentDual203ToeInStrong(mode)
              ? "single BuildRootA + parent dual 203+toeInStrong@4D8BF0"
              : IsOursFpSameTickParentDual203ToeIn(mode)
              ? "single BuildRootA + parent dual 203+toeIn@4D8BF0"
              : IsOursFpSameTickParentDual203Outer(mode)
              ? "single BuildRootA + parent dual 203-OUTER@4D8E80"
              : IsOursFpSameTickParentDualTry6(mode) ? "single BuildRootA + parent dual TRY6@4DA2BA"
              : IsOursFpSameTickParentDualTry5(mode) ? "single BuildRootA + parent dual TRY5@541DC0"
              : IsOursFpSameTickParentDualTry4(mode) ? "single BuildRootA + parent dual TRY4@309D0"
              : IsOursFpSameTickParentDualTry3(mode) ? "single BuildRootA + parent dual TRY3@4DDD50"
              : IsOursFpSameTickParentDualTry2(mode) ? "single BuildRootA + parent dual TRY2@4DE020"
              : IsOursFpSameTickParentDualPose(mode) ? "single BuildRootA + parent dual+poseLatch"
              : IsOursFpSameTickParentDualVs(mode) ? "single BuildRootA + parent dual VS"
              : IsOursFpSameTickParentDualFreeze(mode) ? "single BuildRootA + parent dual+freeze"
              : IsOursFpSameTickParentDual(mode) ? "single BuildRootA + parent dual"
              : IsOursFpSameTickParentCount(mode) ? "BuildRootA x2 + parent COUNT"
              : IsOursFpSameTickParentProbe(mode) ? "BuildRootA x2 + parent probe"
              : IsOursFpSameTickOwnerCount(mode) ? "BuildRootA x2 + owner COUNT"
              : IsOursFpSameTickOwnerObserve(mode) ? "BuildRootA x2 + owner OBS(off)"
              : IsOursFpSameTickDrawRight(mode)    ? "BuildRootA+DrawR same-tick"
              : IsOursFpSameTickPhaseRight(mode) ? "BuildRootA+PhaseR same-tick"
              : IsOursFpSameTickBuildDual(mode)  ? "BuildRootA x2 same-tick"
                                                 : "Mode120 DrawScene dual",
          detail, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      if (IsOursFpDeferredHeadBone(mode))
        Log("StereoRender: kill-switch - stereo=204 (fusion LKG) / 203 / 0");
      else if (IsOursFpSameTickParentDual203CloseIpdToeIn(mode))
        Log("StereoRender: kill-switch - stereo=214 (IPD 0.25 no toe-in) / 203 / 0");
      else if (IsOursFpSameTickParentDual203CloseIpdQtr(mode))
        Log("StereoRender: kill-switch - stereo=213 (IPD 0.5cm) / 203 / 0");
      else if (IsOursFpSameTickParentDual203CloseIpdHalf(mode))
        Log("StereoRender: kill-switch - stereo=212 (IPD 1cm) / 203 / 0");
      else if (IsOursFpSameTickParentDual203CloseIpd(mode) ||
          IsOursFpSameTickParentDual203ToeIn(mode) || IsOursFpSameTickParentDual203Outer(mode))
        Log("StereoRender: kill-switch - stereo=203 (MILESTONE) / 204 / 0");
      else if (IsOursFpSameTickParentDualTry6(mode) || IsOursFpSameTickParentDualTry5(mode) ||
          IsOursFpSameTickParentDualTry4(mode) || IsOursFpSameTickParentDualTry3(mode))
        Log("StereoRender: kill-switch - stereo=204 (fusion LKG) / 203 / 0");
      else if (IsOursFpSameTickParentDualTry2(mode))
        Log("StereoRender: kill-switch - stereo=203 (MILESTONE 3D VR) / 0");
      else if (IsOursFpSameTickParentDualPose(mode))
        Log("StereoRender: kill-switch - stereo=200 / 191 / 0");
      else if (IsOursFpSameTickParentDualVs(mode))
        Log("StereoRender: kill-switch - stereo=200 / 191 / 0 — watch vsPatch>0");
      else if (IsOursFpSameTickParentDualFreeze(mode))
        Log("StereoRender: kill-switch - stereo=200 (smooth parent dual) / 191 / 0");
      else if (IsOursFpSameTickParentDual(mode))
        Log("StereoRender: kill-switch - stereo=191 (MILESTONE) / 0");
      else if (IsOursFpSameTickParentCount(mode))
        Log("StereoRender: kill-switch - stereo=191 (MILESTONE) / 0");
      else if (IsOursFpSameTickParentProbe(mode))
        Log("StereoRender: kill-switch - stereo=191 (MILESTONE) / 0");
      else if (IsOursFpSameTickOwnerCount(mode))
        Log("StereoRender: kill-switch - stereo=191 (MILESTONE) / 0");
      else if (IsOursFpSameTickOwnerObserve(mode))
        Log("StereoRender: kill-switch - stereo=191 (MILESTONE) / 0");
      else if (IsOursFpSameTickDrawRight(mode))
        Log("StereoRender: kill-switch - stereo=191 (MILESTONE) / 192 / 0");
      else if (IsOursFpSameTickPhaseRight(mode))
        Log("StereoRender: kill-switch - stereo=191 (MILESTONE) / 187 / 0");
      else if (IsOursFpSameTickBuildDual(mode))
        Log("StereoRender: kill-switch - stereo=187 / 185 / 0 (191=MILESTONE LKG)");
      else
        Log("StereoRender: kill-switch - stereo=162 / 161 / 158 or 0");
    } else if (mode == StereoMode::CleanDualAlwaysFresh) {
      Log("StereoRender: mode %d CLEAN ALWAYS-FRESH (everyN=1 dual; look→BB→L+R EVERY; "
          "POSEHOLD→fresh monolook; hitch→LIVELOOK; NEVER bare HOLD; NOT same-tex; "
          "hitch 32; no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualLateLatch) {
      Log("StereoRender: mode %d CLEAN LATE-LATCH (Mode120 content dualn=2 HOLD; "
          "NO monolook; every Submit TextureWithPose pose sampled immediately "
          "before Submit; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualMonoLookBack132) {
      Log("StereoRender: mode %d CLEAN BACK-132 FAILED HOLD freeze (prefer 135; BB→L AND "
          "BB→R EVERY EndScene monolook; 132 pitch/hyst; NOT same-tex; calm POSEHOLD; "
          "dualn=3; hitch 32; no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualMonoLookHmdCheap) {
      Log("StereoRender: mode %d CLEAN MONOLOOK-HMD-CHEAP FAILED (same-tex L+R; "
          "prefer 134; BB→L only + Submit same tex; calm POSEHOLD; dualn=3; hitch 32; "
          "no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualMonoLookHmdEvery) {
      Log("StereoRender: mode %d CLEAN MONOLOOK-HMD-EVERY (131 pitch-stable; "
          "BB→both EVERY EndScene; calm POSEHOLD last stereo; dualn=3; hitch 32; "
          "no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualMonoLookPitch) {
      Log("StereoRender: mode %d CLEAN MONOLOOK-PITCH (130 monolook; pitch≥2.5° + "
          "sustain3; ≥600ms hyst; BB→both every 3 frames; calm POSEHOLD last stereo; "
          "dualn=3; hitch 32; no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualMonoLook) {
      Log("StereoRender: mode %d CLEAN MONOLOOK (128 fill/tight; look→identical BB "
          "both eyes ≥350ms hyst; calm dualn=3 HOLD; pose extends mono; hitch 32; "
          "no LOOK-FORCE dual; no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualLrSync) {
      Log("StereoRender: mode %d CLEAN L/R SYNC (128 fill/tight; look→force same-tick "
          "dual; calm dualn=3 HOLD; pose→mono BB ≥200ms hyst; hitch 32; no mismatched "
          "HOLD pair; no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualFpsHoldStereo) {
      Log("StereoRender: mode %d CLEAN FPS NO-LIVELOOK (127 fill + POSEHOLD≥12ms/5 "
          "HARD≥24ms/8; dualn=3; hitch 32; pose→HOLD last stereo; no look-rate "
          "LIVELOOK; no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualFpsPoseTight) {
      Log("StereoRender: mode %d CLEAN FPS+LIVELOOK TIGHT (126 path + POSEHOLD≥12ms/5 "
          "HARD≥24ms/8; dualn=3; hitch 32; no stale LOOKHOLD; no AER; no FPS HUD) "
          "ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualFpsLiveLook) {
      Log("StereoRender: mode %d CLEAN FPS+LIVELOOK (123 fill + POSEHOLD + live BB "
          "both eyes; no stale LOOKHOLD; no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualPerfLookHold) {
      Log("StereoRender: mode %d CLEAN PERF LOOKHOLD (dualn=4; look/pose HOLD; "
          "no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualPerfFillCombo) {
      Log("StereoRender: mode %d CLEAN PERF+FILL COMBO (dualn=4 LOOKHOLD + "
          "cover-matched fovadd; no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualFillMatch) {
      Log("StereoRender: mode %d CLEAN FILLMATCH (cover-matched fovadd; H≈100%%; "
          "no SoftInset; no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualZoomScale) {
      Log("StereoRender: mode %d CLEAN ZOOMSCALE (MatchH6 default; F7 dezoom ladder; "
          "no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else if (mode == StereoMode::CleanDualLookMoveDualN3) {
      Log("StereoRender: mode %d CLEAN DUAL+LOOKMOVE dualn=3 EXPERIMENT (HOLD; "
          "same as 120; no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=120 (good), 51, 45, or 0");
    } else {
      Log("StereoRender: mode %d CLEAN DUAL+LOOKMOVE LKG (DrawScene×2 everyN + HOLD; "
          "head-owned; pedCoupled=0; motionGuard=0; no AER; no FPS HUD) ok=%d fovSite=%d",
          mid, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=51, 45, or 0");
    }
    return g_ok.load();
  }

  if (mode == StereoMode::PhaseDualDeviceVs || mode == StereoMode::FovRecomputeSite ||
      mode == StereoMode::FovRecomputeTrueCanvas || mode == StereoMode::FovCanvasComfort ||
      mode == StereoMode::AerPoseSubmit || mode == StereoMode::FovCanvasLowMotion ||
      mode == StereoMode::FovCanvasMotionGuard || mode == StereoMode::ReplayCallChainProbe ||
      mode == StereoMode::ReplayOwnerCountProbe ||
      mode == StereoMode::FovCanvasMotionGuardFast ||
      mode == StereoMode::FovCanvasMotionGuardRtLock ||
      mode == StereoMode::HeadOwnedCamSpike ||
      mode == StereoMode::HeadOwnedCamFullPose ||
      mode == StereoMode::HeadOwnedCamLeveledPitchFlip ||
      mode == StereoMode::HeadOwnedCamPitchStable ||
      mode == StereoMode::HeadOwnedCamPedCoupled ||
      mode == StereoMode::HeadOwnedCamStereoAlways ||
      mode == StereoMode::HeadOwnedCamStereoAer ||
      mode == StereoMode::HeadOwnedCamStereoSwap ||
      mode == StereoMode::HeadOwnedCamStereoSoftGuard) {
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
    g_holdPoseLValid = g_holdPoseRValid = false;
    g_submitPoseLValid = g_submitPoseRValid = false;
    g_vsPatchOn.store(false);
    ClearGameFovPairLatch();
    g_ok = InstallRootProbeHooks(2);
    if (mode == StereoMode::AerPoseSubmit) {
      const bool fovOk = InstallFovRecomputeSiteHook();
      Log("StereoRender: mode 38 AER-POSE SUBMIT (Mode37 true-FOV + pair-hold + "
          "Submit_TextureWithPose per eye; Luke AER lesson; no OF/v2) ok=%d fovSite=%d",
          g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=37 or 30 (+ delete fovadd)");
    } else if (mode == StereoMode::FovCanvasComfort) {
      const bool fovOk = InstallFovRecomputeSiteHook();
      Log("StereoRender: mode 37 FOV-CANVAS COMFORT (Mode36 true-FOV + Mode30 pair-hold; "
          "fovadd~18; canvasCap=1536; pair-latch tangents; WorldScale for size; no CanvasZoom) "
          "ok=%d fovSite=%d",
          g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=36 or 35 (keep fovadd) or stereo=30 + delete "
          "gtaiv_dxvk_vr.fovadd");
    } else if (mode == StereoMode::FovCanvasLowMotion) {
      const bool fovOk = InstallFovRecomputeSiteHook();
      Log("StereoRender: mode 39 LOW-MOTION COMFORT (Mode37 true-FOV canvas + pair-hold; "
          "fovadd capped at 12deg; no pose submit; no CanvasZoom) ok=%d fovSite=%d",
          g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=38/37 (restore fovadd behavior) or stereo=30 "
          "+ delete gtaiv_dxvk_vr.fovadd");
    } else if (mode == StereoMode::FovCanvasMotionGuard) {
      const bool fovOk = InstallFovRecomputeSiteHook();
      g_mode40MonoPairs.store(0);
      Log("StereoRender: mode 40 MOTION-GUARD (Mode37 true-FOV canvas + pair-hold; rapid "
          "HMD L/R delta -> current-frame mono pair, then stereo resumes; not true stereo) "
          "ok=%d fovSite=%d",
          g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=37 (always stereo) or stereo=30 + delete "
          "gtaiv_dxvk_vr.fovadd");
    } else if (mode == StereoMode::FovCanvasMotionGuardFast) {
      const bool fovOk = InstallFovRecomputeSiteHook();
      g_mode40MonoPairs.store(0);
      Log("StereoRender: mode 43 FAST MOTION-GUARD (Mode40 visual behavior; direct submit "
          "canvases on guard = 3 copies instead of hold recanvas + promotion 5; no replay hook, "
          "not true stereo) ok=%d fovSite=%d",
          g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: kill-switch - stereo=40 (conservative guard), 37, or 30 + delete "
          "gtaiv_dxvk_vr.fovadd");
    } else if (mode == StereoMode::FovCanvasMotionGuardRtLock ||
               mode == StereoMode::HeadOwnedCamSpike ||
               mode == StereoMode::HeadOwnedCamFullPose ||
               mode == StereoMode::HeadOwnedCamLeveledPitchFlip ||
               mode == StereoMode::HeadOwnedCamPitchStable ||
               mode == StereoMode::HeadOwnedCamPedCoupled ||
               mode == StereoMode::HeadOwnedCamStereoAlways ||
               mode == StereoMode::HeadOwnedCamStereoAer ||
               mode == StereoMode::HeadOwnedCamStereoSwap ||
      mode == StereoMode::HeadOwnedCamStereoSoftGuard) {
      const bool fovOk = InstallFovRecomputeSiteHook();
      g_mode40MonoPairs.store(0);
      g_mode44RtChecks.store(0);
      g_mode44RtRecreates.store(0);
      g_mode44RtSuppressed.store(0);
      if (UsesStereoAlwaysDistinct(mode)) {
        g_stereoAuditPairs.store(0);
        g_stereoAuditZero.store(0);
        g_stereoAuditDiffSum.store(0);
        if (mode == StereoMode::HeadOwnedCamStereoAer) {
          Log("StereoRender: mode 51 STEREO-AER (Mode50 always-distinct L/R + "
              "Submit_TextureWithPose; motion-guard OFF; not same-frame) ok=%d fovSite=%d",
              g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
          Log("StereoRender: kill-switch - stereo=50, 49, or 45 + delete fovadd");
        } else if (mode == StereoMode::HeadOwnedCamStereoSoftGuard) {
          g_mode53SoftGuard.store(0);
          g_mode53FullMono.store(0);
          Log("StereoRender: mode 53 STEREO-SOFT-GUARD (Mode51 AER + distinct L/R; "
              "8deg/6cm soft=AER no flatten; 15deg/12cm hard=full mono only) ok=%d fovSite=%d",
              g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
          Log("StereoRender: kill-switch - stereo=51, 50, 49, or 45 + delete fovadd");
        } else if (mode == StereoMode::HeadOwnedCamStereoSwap) {
          Log("StereoRender: mode 52 STEREO-SWAP (Mode50 + swapped L/R submit — depth "
              "inversion test; motion-guard OFF) ok=%d fovSite=%d",
              g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
          Log("StereoRender: kill-switch - stereo=50, 49, or 45 + delete fovadd");
        } else {
          Log("StereoRender: mode 50 STEREO-ALWAYS (Mode49 ped-coupled head-owned but "
              "motion-guard OFF — always distinct temporal L/R when ipd>0; StereoAudit "
              "logs L≠R proof; not same-frame) ok=%d fovSite=%d",
              g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
          Log("StereoRender: kill-switch - stereo=49 (motion-guard baseline), 45, or 30 "
              "+ delete gtaiv_dxvk_vr.fovadd");
        }
      } else if (mode == StereoMode::HeadOwnedCamSpike ||
          mode == StereoMode::HeadOwnedCamFullPose ||
          mode == StereoMode::HeadOwnedCamLeveledPitchFlip ||
          mode == StereoMode::HeadOwnedCamPitchStable ||
          mode == StereoMode::HeadOwnedCamPedCoupled) {
        if (mode == StereoMode::HeadOwnedCamPedCoupled) {
          Log("StereoRender: mode 49 PED-COUPLED HEAD-OWNED (Mode48 pitch-stable + pre-capture "
              "refresh; leveledPitchFlip=OFF; yaw follows ped + HMD delta; no Mode-46 roll; "
              "not true stereo) ok=%d fovSite=%d",
              g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
          Log("StereoRender: kill-switch - stereo=48, 45, 47, 44, 37, or 30 + delete "
              "gtaiv_dxvk_vr.fovadd");
        } else if (mode == StereoMode::HeadOwnedCamPitchStable) {
          Log("StereoRender: mode 48 HEAD-OWNED PITCH-STABLE (Mode47 + stable up/down basis "
              "+ pre-capture CopyMat refresh; no Mode-46 roll; not true stereo) ok=%d fovSite=%d",
              g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
          Log("StereoRender: kill-switch - stereo=47, 45, 44, 37, or 30 + delete "
              "gtaiv_dxvk_vr.fovadd");
        } else {
          Log("StereoRender: mode %d HEAD-OWNED LEVELED-PITCH (Mode44 RT lock + post-CCam "
              "CopyMat HMD reapply; Mode46 keeps unsafe roll; Mode47 pitch flip ON; no "
              "collision/VS/replay change; not true stereo) ok=%d fovSite=%d",
              static_cast<int>(mode),
              g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
          Log("StereoRender: kill-switch - stereo=45, 44, 37, or 30 + delete "
              "gtaiv_dxvk_vr.fovadd");
        }
      } else {
        Log("StereoRender: mode 44 RT-LOCK (Mode43 fast motion guard + locked 1536 eye RTs; "
            "5%% FOV tangent publish gate; no FOV/RT thrash; not true stereo) ok=%d fovSite=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
        Log("StereoRender: kill-switch - stereo=43, 40, 37, or 30 + delete "
            "gtaiv_dxvk_vr.fovadd");
      }
    } else if (mode == StereoMode::ReplayCallChainProbe) {
      const bool fovOk = InstallFovRecomputeSiteHook();
      g_mode40MonoPairs.store(0);
      g_mode41FrameN = 0;
      g_mode41AggN = 0;
      g_mode41VsRetHits.store(0);
      g_mode41Es.store(0);
      g_mode41VsTid = 0;
      Log("StereoRender: mode 41 REPLAY-CHAIN PROBE (Mode40 true-FOV pair-hold + motion "
          "guard; READ-ONLY VsRet=0x2C73E stack/call map; no replay hook, no dual draw) "
          "ok=%d fovSite=%d",
          g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: Mode41 cannot claim SameFrame/DISTINCT eyes; kill-switch - stereo=40, "
          "37, or 30 + delete gtaiv_dxvk_vr.fovadd");
    } else if (mode == StereoMode::ReplayOwnerCountProbe) {
      const bool fovOk = InstallFovRecomputeSiteHook();
      g_mode40MonoPairs.store(0);
      g_mode41FrameN = 0;
      g_mode41AggN = 0;
      g_mode41VsRetHits.store(0);
      g_mode41Es.store(0);
      g_mode41VsTid = 0;
      Log("StereoRender: mode 42 INDIRECT-CALL PROBE (Mode40 true-FOV pair-hold + motion "
          "guard; READ-ONLY FF /2 decoder around 0x309D0; no game hook, no replay) "
          "ok=%d fovSite=%d",
          g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
      Log("StereoRender: Mode42 cannot claim SameFrame/DISTINCT eyes; kill-switch - stereo=41, "
          "40, 37, or 30 + delete gtaiv_dxvk_vr.fovadd");
    } else if (mode == StereoMode::FovRecomputeTrueCanvas) {
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

  // Mode216 bone sample moved to HookDrawWalk (after dual warm) — EndScene-during-
  // load called GetBonePos too early and crashed after the load screen.

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

    // Mode 120–136 / 140: Dual path owns L/R via DrawScene (runs before EndScene).
    // HOLD → skip TemporalCapture. Mode126/127 LIVELOOK + Mode129–135 mono →
    // StretchRect live BB (Mode131 only: every 3rd mono frame; Mode133: BB→L once FAILED).
    // Mode 135: liveLook always BB→L AND BB→R (132 style); never same-tex.
    if (UsesDrawSceneDualPath(mode)) {
      // Mode 191: BuildRootA already captured L/R this tick — do not StretchRect over them.
      if (IsOursFpSameTickBuildDual(mode)) {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
          D3DSURFACE_DESC desc{};
          bb->GetDesc(&desc);
          bb->Release();
          uint32_t rtW = desc.Width, rtH = desc.Height;
          ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
          EnsureEyeRts(device, rtW, rtH);
        }
        if (IsOursFpSameTickParentDual(mode))
          Mode200OnEndScene();
        else if (IsOursFpSameTickParentCount(mode))
          Mode199OnEndScene();
        else if (IsOursFpSameTickParentProbe(mode))
          Mode198OnEndScene();
        else if (IsOursFpSameTickOwnerCount(mode))
          Mode197OnEndScene();
        return;
      }
      // Mode 174/175: never StretchRect to eye canvases — BB Submit owns the HMD image.
      // Mode 177: DrawScene AER owns capture — skip EndScene StretchRect/HOLD.
      if (IsOursFpBbPhone(mode) || IsOursFpAer(mode))
        return;
      g_dualDoneThisFrame = false;
      IDirect3DSurface9* bb = nullptr;
      if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return;
      D3DSURFACE_DESC desc{};
      bb->GetDesc(&desc);
      bb->Release();
      uint32_t rtW = desc.Width, rtH = desc.Height;
      ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
      EnsureEyeRts(device, rtW, rtH);
      const bool liveLook = StereoDualLiveLookActive();
      StereoDualClearLiveLook();
      const bool hold = StereoDualHoldActive();
      StereoDualClearHold();
      // Mode 131 mono streak: reset when not in liveLook so next enter forces BB copy.
      // Mode 132/134/135 deliberately have NO throttle — StretchRect×2 every mono EndScene.
      // Mode 133 same-tex path kept for A/B only (FAILED fusion).
      static bool s_mono131Streak = false;
      if (!liveLook) {
        s_mono131Streak = false;
        g_submitSameLBoth = false;
      }
      if (liveLook) {
        // Mode 131 ONLY: StretchRect BB→L+R every EndScene was expensive (Mode130 dips
        // ~50–56 FPS during monolook). Refresh live BB every 3rd mono frame; reuse
        // last identical L/R otherwise. FAILED: HMD starved while Present stayed ~90.
        // Mode 132/134/135: skip this throttle — every-frame StretchRect×2 during mono.
        // Mode 133: every-frame ONE StretchRect BB→L; Submit same tex for both eyes (FAILED).
        if (IsCleanDualMonoLookPitch(mode)) {
          if (s_mono131Streak && g_haveL && g_haveR) {
            static uint32_t s_monoRefresh = 0;
            if ((++s_monoRefresh % 3u) != 0u)
              return;
          } else {
            s_mono131Streak = true;
          }
        }
        if (IsCleanDualMonoLookHmdCheap(mode)) {
          // FAILED path kept for A/B: BB→L once; OpenVR Submit same handle L+R.
          const bool okL = CopyBbToEyeCanvasGated(device, g_texL, vr::Eye_Left);
          if (okL) {
            g_haveL = g_haveR = true;
            g_submitSameLBoth = true;
          }
          static uint32_t s_cheapN = 0;
          const uint32_t n = ++s_cheapN;
          if (n <= 6 || (n % 300) == 0)
            Log("Mode133: EndScene LIVELOOK BB→L same-Submit #%u ok=%d", n, okL ? 1 : 0);
          return;
        }
        // Mode 132/134/135 (+130/129 LIVELOOK): same BB → L and R (never same-tex Submit).
        g_submitSameLBoth = false;
        const bool okL = CopyBbToEyeCanvasGated(device, g_texL, vr::Eye_Left);
        const bool okR = CopyBbToEyeCanvasGated(device, g_texR, vr::Eye_Right);
        if (okL && okR)
          g_haveL = g_haveR = true;
        static uint32_t s_liveN = 0;
        const uint32_t n = ++s_liveN;
        if (n <= 6 || (n % 300) == 0)
          Log("Mode%d: EndScene LIVELOOK BB→both eyes #%u ok=%d",
              static_cast<int>(mode), n, (okL && okR) ? 1 : 0);
        return;
      }
      if (hold || (g_haveL && g_haveR))
        return;
      // Pre-gate mono: wait for first dual; do not AER temporal.
      return;
    }

    if (mode == StereoMode::PhaseDualDeviceVs || mode == StereoMode::FovRecomputeSite ||
        mode == StereoMode::FovRecomputeTrueCanvas || mode == StereoMode::FovCanvasComfort ||
        mode == StereoMode::AerPoseSubmit || mode == StereoMode::FovCanvasLowMotion ||
        mode == StereoMode::FovCanvasMotionGuard || mode == StereoMode::ReplayCallChainProbe ||
        mode == StereoMode::ReplayOwnerCountProbe ||
        mode == StereoMode::FovCanvasMotionGuardFast ||
        mode == StereoMode::FovCanvasMotionGuardRtLock ||
        mode == StereoMode::HeadOwnedCamSpike ||
        mode == StereoMode::HeadOwnedCamFullPose ||
        mode == StereoMode::HeadOwnedCamLeveledPitchFlip ||
        mode == StereoMode::HeadOwnedCamPitchStable ||
        mode == StereoMode::HeadOwnedCamPedCoupled ||
      mode == StereoMode::HeadOwnedCamStereoAlways ||
      mode == StereoMode::HeadOwnedCamStereoAer ||
      mode == StereoMode::HeadOwnedCamStereoSwap ||
      mode == StereoMode::HeadOwnedCamStereoSoftGuard) {
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
      if (mode == StereoMode::ReplayCallChainProbe ||
          mode == StereoMode::ReplayOwnerCountProbe)
        Mode41OnEndScene(device);
      else
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
  // Mode 174: never submit eye canvases — TryMonoSubmit uses raw BB + phone bounds.
  if (IsOursFpBbPhone(mode))
    return false;
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
      mode != StereoMode::FovRecomputeSite && mode != StereoMode::FovRecomputeTrueCanvas &&
      mode != StereoMode::FovCanvasComfort && mode != StereoMode::AerPoseSubmit &&
      mode != StereoMode::FovCanvasLowMotion && mode != StereoMode::FovCanvasMotionGuard &&
      mode != StereoMode::ReplayCallChainProbe && mode != StereoMode::ReplayOwnerCountProbe &&
      mode != StereoMode::FovCanvasMotionGuardFast &&
      mode != StereoMode::FovCanvasMotionGuardRtLock &&
      mode != StereoMode::HeadOwnedCamSpike &&
      mode != StereoMode::HeadOwnedCamFullPose &&
      mode != StereoMode::HeadOwnedCamLeveledPitchFlip &&
      mode != StereoMode::HeadOwnedCamPitchStable &&
      mode != StereoMode::HeadOwnedCamPedCoupled &&
      mode != StereoMode::HeadOwnedCamStereoAlways &&
      mode != StereoMode::HeadOwnedCamStereoAer &&
      mode != StereoMode::HeadOwnedCamStereoSwap &&
      mode != StereoMode::HeadOwnedCamStereoSoftGuard &&
      !UsesDrawSceneDualPath(mode))
    return false;
  if (!device || !interop || !g_texL || !g_texR)
    return false;
  if (!IsStereoRenderArmed())
    return false;
  if (!g_haveL || !g_haveR)
    return false;

  interop->FlushRenderingCommands();
  interop->LockSubmissionQueue();
  bool okL = false, okR = false;
  // Mode 173: always same Vulkan image both eyes (true mono — no Left-layout→Right rivalry).
  const bool sameL = g_submitSameLBoth || IsOursFpSameLPhone(mode);
  if (mode == StereoMode::FusionSwap || mode == StereoMode::HeadOwnedCamStereoSwap) {
    // Diagnostic: feed captured-Left texture to Right eye and vice versa.
    okL = SubmitEyeTexture(device, g_texR, vr::Eye_Left, interop);
    okR = SubmitEyeTexture(device, g_texL, vr::Eye_Right, interop);
  } else if (sameL) {
    // Mode 133 FAILED monolook: same Vulkan image for both eyes (kept for A/B only).
    okL = SubmitEyeTexture(device, g_texL, vr::Eye_Left, interop);
    okR = SubmitEyeTexture(device, g_texL, vr::Eye_Right, interop);
    g_submitSameLBoth = false;
  } else {
    okL = SubmitEyeTexture(device, g_texL, vr::Eye_Left, interop);
    okR = SubmitEyeTexture(device, g_texR, vr::Eye_Right, interop);
  }
  interop->ReleaseSubmissionQueue();

  static uint32_t s_n = 0;
  if ((++s_n) <= 8 || (s_n % 120) == 0)
    Log("StereoSubmit: L=%d R=%d mode=%d swap=%d sameL=%d", okL ? 1 : 0, okR ? 1 : 0,
        static_cast<int>(mode),
        (mode == StereoMode::FusionSwap || mode == StereoMode::HeadOwnedCamStereoSwap) ? 1 : 0,
        sameL ? 1 : 0);
  return okL && okR;
}

bool StereoInDualPass() {
  return g_inDual.load();
}

bool StereoInParentDualWalk() {
  return g_inDrawWalkDual.load();
}

bool StereoParentDualReadyForBone() {
  // Crash after load: GetBonePos before world/ped skeleton ready kills the process.
  // Wait for dual walk to prove the draw shell is alive first.
  return g_mode200HookOk.load() && !g_mode200TooHot.load() &&
         g_mode200DualN.load() >= 5 && g_mode200Es.load() >= 30;
}

bool StereoMode193SkipDrawActive() {
  const DWORD until = g_mode193SkipDrawUntilMs.load();
  return until != 0 && GetTickCount() < until;
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

bool StereoMode41WantsTrace() {
  const StereoMode mode = GetStereoMode();
  // Mode 196 observe DISABLED — VirtualQuery-per-VsRet caused ~1 FPS + look crashes.
  // Evidence already captured (target 0x220D0). Mode 41/42 temporal probes still OK.
  return mode == StereoMode::ReplayCallChainProbe || mode == StereoMode::ReplayOwnerCountProbe;
}

void StereoMode41CollectVsRetChain(void* retAddr, void* hookSpWords) {
  if (!StereoMode41WantsTrace() || !hookSpWords || !retAddr)
    return;
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uint32_t retRva =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(retAddr) - base);
  // Restrict tracing to the real replay uploader. This keeps the hot path small
  // and avoids conflating unrelated D3D9 VS uploads with the Rage replay chain.
  if (retRva != kMode34VsRetRva)
    return;

  ++g_mode41VsRetHits;
  const uint32_t tid = GetCurrentThreadId();
  if (!g_mode41VsTid)
    g_mode41VsTid = tid;
  auto* sp = reinterpret_cast<uintptr_t*>(hookSpWords);
  for (int i = 1; i < 64; ++i) {
    const uintptr_t v = sp[i];
    if (v < base + 0x1000 || v > base + 0xC00000)
      continue;
    Mode41NoteFrameNode(static_cast<uint32_t>(v - base), static_cast<uint8_t>(i));
  }

  static int s_sample = 0;
  if (s_sample < 8) {
    ++s_sample;
    Log("%s: VsRet sample#%d tid=%u (stack slots retained; no hook/dual)",
        GetStereoMode() == StereoMode::ReplayOwnerCountProbe ? "Mode42" : "Mode41", s_sample,
        tid);
  }
}

bool StereoMode198WantsSample() {
  return IsOursFpSameTickParentProbe(GetStereoMode()) && g_mode198ArmSample.load();
}

void StereoMode198CollectVsRet(void* retAddr, void* hookSpWords) {
  if (!StereoMode198WantsSample() || !hookSpWords || !retAddr)
    return;
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uint32_t retRva =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(retAddr) - base);
  if (retRva != 0x2C73E)
    return;
  // Claim the once-per-EndScene sample slot immediately (no VQ, no loops beyond stack).
  if (!g_mode198ArmSample.exchange(false))
    return;
  ++g_mode198Samples;
  auto* sp = reinterpret_cast<uintptr_t*>(hookSpWords);
  g_mode198FrameN = 0;
  for (int i = 1; i < 48 && g_mode198FrameN < 48; ++i) {
    const uintptr_t v = sp[i];
    if (v < base + 0x1000 || v > base + 0xC00000)
      continue;
    const uint32_t rva = static_cast<uint32_t>(v - base);
    bool dup = false;
    for (int j = 0; j < g_mode198FrameN; ++j) {
      if (g_mode198FrameRva[j] == rva) {
        dup = true;
        break;
      }
    }
    if (!dup)
      g_mode198FrameRva[g_mode198FrameN++] = rva;
  }
  static int s_n = 0;
  if (s_n < 6) {
    ++s_n;
    Log("Mode198: took 1 stack sample #%d nodes=%d tid=%u (armed next ES)", s_n,
        g_mode198FrameN, GetCurrentThreadId());
  }
}

}  // namespace asi
