#include "stereo_render.h"
#include "aob.h"
#include "cam_matrix.h"
#include "hmd_pose.h"
#include "log.h"
#include "re_validate.h"
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
std::atomic<uint32_t> g_mode44RtChecks{0};
std::atomic<uint32_t> g_mode44RtRecreates{0};
std::atomic<uint32_t> g_mode44RtSuppressed{0};

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
std::atomic<uint32_t> g_mode40MonoPairs{0};

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
         IsHeadOwnedCamFamily(mode);
}

bool UsesTrueFovCanvasPublish(StereoMode mode) {
  return mode == StereoMode::FovRecomputeTrueCanvas || UsesFovComfortPath(mode);
}

void SnapshotHoldPose(bool rightEye) {
  // Freeze to once-per-EndScene sample (BeginFrameHmdPoseSample) — never live
  // resample mid-frame (AER jump root when inject + Submit disagreed).
  vr::HmdMatrix34_t m{};
  if (!GetFrameHmdPoseMatrix(&m) && !GetHmdPoseMatrix(&m)) {
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
bool UsesMotionGuard(StereoMode mode) {
  return mode == StereoMode::FovCanvasMotionGuard || mode == StereoMode::ReplayCallChainProbe ||
         mode == StereoMode::ReplayOwnerCountProbe || mode == StereoMode::FovCanvasMotionGuardFast ||
         mode == StereoMode::FovCanvasMotionGuardRtLock || IsHeadOwnedCamFamily(mode);
}

bool Mode40NeedsMonoGuard(float* outRotDeg, float* outMoveCm) {
  const StereoMode mode = GetStereoMode();
  // Mode 72/73/75 dual: mono-guard copies same BB → kills L≠R. Mode 76 AER: same.
  // Mode 74 hitchcut pair-hold: keep motion guard (family thresholds).
  if (mode == StereoMode::VsConstTemporalInject || mode == StereoMode::SameFrameVsConstDual ||
      mode == StereoMode::SparseSessionDual || mode == StereoMode::AerPresenceExperimental)
    return false;
  if (!UsesMotionGuard(mode) || !g_holdPoseLValid || !g_holdPoseRValid)
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
  // One temporal pair normally spans two EndScene epochs. These thresholds
  // preserve stereo while still, but guard the visibly bad turn/lean case.
  // Mode 45: tighter comfort — fire mono sooner on head turn/lean.
  const bool mode45Family = IsHeadOwnedCamFamily(mode);
  const float rotTh = mode45Family ? 0.8f : 1.5f;
  const float moveTh = mode45Family ? 1.2f : 2.f;
  return rotDeg >= rotTh || moveCm >= moveTh;
}

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

// ---- Mode 64: ViewConst COUNT-only (Mode 45 renderer) ----
// TRUE start mapped RVA 0x32470 (frame+CC-pad). Mid 0x3247C is after sub esp — do not hook mid.
// PASS band [0.8,4] ≈ 1×/frame. dual=OFF; auto-reverts to 45.
constexpr uint32_t kMode64ViewConstExpectedRva = 0x32470;
constexpr uint32_t kMode64CountEsNeed = 45;
static const uint8_t kMode64Prologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC,
                                          0xA8, 0x00, 0x00, 0x00, 0x56, 0x57, 0x8B, 0xF9};
static const char kMode64Pattern[] =
    "55 8B EC 83 E4 F0 81 EC A8 00 00 00 56 57 8B F9 8D 44 24 70 F3 0F 10 87";
std::atomic<uint32_t> g_mode64SiteRva{0};
std::atomic<uint32_t> g_mode64Es{0};
std::atomic<uint32_t> g_mode64CountEs{0};
std::atomic<bool> g_mode64CountDone{false};
uint32_t g_mode64EntrySum = 0;

// ---- Mode 65: read-only [0x17ed8d8] vtable +0x178/+0x1B4 log (Mode 45 renderer) ----
// EndScene peek only — no game hook. Logs replayGate [0x17ED918] and activeView [0x17F583C]
// (PublishSync gate). Compare slot178 RVAs with Mode42 OWNER-EDGE.
constexpr uint32_t kMode65DeviceGlobalVa = 0x17ED8D8;
constexpr uint32_t kMode65ReplayGateVa = 0x17ED918;
constexpr uint32_t kMode65ActiveViewVa = 0x17F583C;
constexpr uint32_t kMode65LogEsNeed = 45;
std::atomic<uint32_t> g_mode65Es{0};
std::atomic<bool> g_mode65LogDone{false};
uint32_t g_mode65LastDevice = 0;
uint32_t g_mode65LastVt178 = 0;
uint32_t g_mode65LastVt1B4 = 0;

// ---- Mode 66: PublishSync COUNT-only (Mode 45 renderer) ----
// Mapped RVA 0x30F00 (old file-offset doc 0x30300). AOB resolve; CC-pad NOT required
// (prior fn epilogue). dual=OFF; auto-reverts to 45.
constexpr uint32_t kMode66PublishSyncExpectedRva = 0x30F00;
constexpr uint32_t kMode66CountEsNeed = 45;
static const uint8_t kMode66Prologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x51, 0x56, 0x8B, 0xF1};
static const char kMode66Pattern[] =
    "55 8B EC 83 E4 F8 51 56 8B F1 8D 86 80 00 00 00 50 8D 86 80 01 00 00 50 8D 8E C0 00 00 00 E8";
std::atomic<uint32_t> g_mode66SiteRva{0};
std::atomic<uint32_t> g_mode66Es{0};
std::atomic<uint32_t> g_mode66CountEs{0};
std::atomic<bool> g_mode66CountDone{false};
uint32_t g_mode66EntrySum = 0;

// ---- Mode 67: ViewMatWriter COUNT-only (Mode 45 renderer) ----
// Mapped RVA 0x314C0 (old file-offset doc 0x308C0). CC-pad OK. dual=OFF.
constexpr uint32_t kMode67ViewMatExpectedRva = 0x314C0;
constexpr uint32_t kMode67CountEsNeed = 45;
static const uint8_t kMode67Prologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x10,
                                          0x56, 0x57, 0x8B, 0xF9};
static const char kMode67Pattern[] =
    "55 8B EC 83 E4 F8 83 EC 10 56 57 8B F9 0F 57 C9 F3 0F 10 87 BC 02 00 00";
std::atomic<uint32_t> g_mode67SiteRva{0};
std::atomic<uint32_t> g_mode67Es{0};
std::atomic<uint32_t> g_mode67CountEs{0};
std::atomic<bool> g_mode67CountDone{false};
uint32_t g_mode67EntrySum = 0;

// ---- Mode 68: ReplayDispatch COUNT-only (Mode 45 renderer) ----
// Mapped 0x30CD0 — only 2 E8 callers (SetActiveView + PublishSync). Calls device
// vtable +0x178 (= live Mode65 slot178 0x220D0 SetVSConstF) with view+0x80 matrix.
// Prologue is cmp/push/mov esi,ecx (thiscall, no frame) — skip frame ABI check.
constexpr uint32_t kMode68ReplayDispatchExpectedRva = 0x30CD0;
constexpr uint32_t kMode68CountEsNeed = 45;
static const uint8_t kMode68Prologue[] = {0x83, 0x3D, 0x18, 0xD9, 0x7E, 0x01, 0x00, 0x56, 0x8B, 0xF1};
static const char kMode68Pattern[] = "83 3D 18 D9 7E 01 00 56 8B F1 75";
std::atomic<uint32_t> g_mode68SiteRva{0};
std::atomic<uint32_t> g_mode68Es{0};
std::atomic<uint32_t> g_mode68CountEs{0};
std::atomic<bool> g_mode68CountDone{false};
uint32_t g_mode68EntrySum = 0;

// ---- Mode 69: ReplayDispatch view+0x80 matrix PEEK (Mode 45 renderer) ----
// Same site as Mode 68. READ-ONLY copy of float[16] at ecx+0x80 (VS const src).
// Never writes game memory; dual=OFF; auto→45 after live matrix + ≥45 EndScenes.
// WAIT phase: ignore identity (menu/load) until |t| or rotation leaves I, or timeout.
constexpr uint32_t kMode69ReplayDispatchExpectedRva = 0x30CD0;
constexpr uint32_t kMode69PeekEsNeed = 45;
constexpr uint32_t kMode69WaitEsMax = 900;  // ~15–30s of EndScenes waiting for live cam
static const uint8_t kMode69Prologue[] = {0x83, 0x3D, 0x18, 0xD9, 0x7E, 0x01, 0x00, 0x56, 0x8B, 0xF1};
static const char kMode69Pattern[] = "83 3D 18 D9 7E 01 00 56 8B F1 75";
std::atomic<uint32_t> g_mode69SiteRva{0};
std::atomic<uint32_t> g_mode69Es{0};
std::atomic<uint32_t> g_mode69PeekEs{0};
std::atomic<uint32_t> g_mode69WaitEs{0};
std::atomic<bool> g_mode69Live{false};
std::atomic<bool> g_mode69PeekDone{false};
std::atomic<uint32_t> g_mode69PeekOk{0};
std::atomic<uint32_t> g_mode69PeekFail{0};
std::atomic<uint32_t> g_mode69LogLeft{8};
float g_mode69LastMat[16]{};
bool g_mode69HaveLast = false;
float g_mode69MaxDelta = 0.f;

// ---- Mode 70: ReplayDispatch ACTIVE-VIEW filtered peek (Mode 45 renderer) ----
// Same site as 68/69. Only samples when self == *[0x17F583C] AND matrix looks live.
// READ-ONLY; dual=OFF. Success leaves stereo=70 (no force-45).
constexpr uint32_t kMode70ReplayDispatchExpectedRva = 0x30CD0;
constexpr uint32_t kMode70PeekEsNeed = 45;
constexpr uint32_t kMode70WaitEsMax = 900;
static const uint8_t kMode70Prologue[] = {0x83, 0x3D, 0x18, 0xD9, 0x7E, 0x01, 0x00, 0x56, 0x8B, 0xF1};
static const char kMode70Pattern[] = "83 3D 18 D9 7E 01 00 56 8B F1 75";
std::atomic<uint32_t> g_mode70SiteRva{0};
std::atomic<uint32_t> g_mode70WaitEs{0};
std::atomic<uint32_t> g_mode70PeekEs{0};
std::atomic<bool> g_mode70Live{false};
std::atomic<bool> g_mode70PeekDone{false};
std::atomic<uint32_t> g_mode70LiveTotal{0};
std::atomic<uint32_t> g_mode70ActiveHit{0};
std::atomic<uint32_t> g_mode70ActiveMiss{0};
std::atomic<uint32_t> g_mode70LogLeft{8};
float g_mode70LastMat[16]{};
bool g_mode70HaveLast = false;
float g_mode70MaxDelta = 0.f;

// ---- Mode 71: temporal L/R inject at ReplayDispatch (activeView+live) ----
// WRITE view+0x80 translation ±(sep*stereoScale)/2 along right row, restore after call.
// Mode-45 TemporalCapturePairHold captures alternating eyes. SameFrameSeamGate=CLOSED.
// Safety (freeze 2026-07-25): unit right only, finite floats, offset cap, 1 inject/ES,
// always restore even if call faults.
constexpr uint32_t kMode71ReplayDispatchExpectedRva = 0x30CD0;
static const uint8_t kMode71Prologue[] = {0x83, 0x3D, 0x18, 0xD9, 0x7E, 0x01, 0x00, 0x56, 0x8B, 0xF1};
static const char kMode71Pattern[] = "83 3D 18 D9 7E 01 00 56 8B F1 75";
std::atomic<uint32_t> g_mode71SiteRva{0};
std::atomic<bool> g_mode71Armed{false};
std::atomic<bool> g_mode71Dead{false};
std::atomic<uint32_t> g_mode71Injects{0};
std::atomic<uint32_t> g_mode71Skip{0};
std::atomic<uint32_t> g_mode71RejectMat{0};
std::atomic<uint32_t> g_mode71Budget{0};  // set to 1 each EndScene; consume one inject
constexpr float kMode71MaxHalfSep = 0.25f;  // metres cap
constexpr float kMode71MinT2 = 10.f;        // require real world translation

// ---- Mode 72: SetVSConstF src-copy inject (never writes live view) ----
// Hook mapped 0x220D0 (Mode65 slot178). When src == activeView+0x80 and matrix sane,
// copy to local buf, ±IPD/2 along right, call orig with local ptr. 1 inject/ES.
constexpr uint32_t kMode72SetVSConstExpectedRva = 0x220D0;
static const uint8_t kMode72Prologue[] = {0x8B, 0x54, 0x24, 0x04, 0x57, 0x8B, 0x7C, 0x24, 0x14};
static const char kMode72Pattern[] = "8B 54 24 04 57 8B 7C 24 14 8D 04 BD 03 00 00 00";
using Mode72SetVSConstF_t = void(__stdcall*)(void* device, uint32_t startReg, const float* src,
                                             uint32_t count);
Mode72SetVSConstF_t g_origMode72SetVS = nullptr;
void* g_mode72HookAddr = nullptr;
std::atomic<uint32_t> g_mode72SiteRva{0};
std::atomic<bool> g_mode72Armed{false};
std::atomic<bool> g_mode72Dead{false};
std::atomic<uint32_t> g_mode72Injects{0};
std::atomic<uint32_t> g_mode72Skip{0};
std::atomic<uint32_t> g_mode72RejectMat{0};
std::atomic<uint32_t> g_mode72Budget{0};
float g_mode72LocalMat[16]{};
// Mode74 per-eye HMD projection (src-copy / device VS patch). Never writes view+0x80.
float g_mode74LocalProj[16]{};
float g_mode74CachedProjL[16]{};
float g_mode74CachedProjR[16]{};
std::atomic<bool> g_mode74ProjCacheOk{false};
std::atomic<uint32_t> g_mode74ProjInjects{0};
std::atomic<uint32_t> g_mode74ProjDevicePatches{0};
std::atomic<uint32_t> g_mode74ProjMiss{0};
std::atomic<uint32_t> g_mode74ProjDiscoverN{0};
// PublishProj mapped 0x31BA0 — builds frustum block at view+0x308 from +0x180.
constexpr uint32_t kMode74PublishProjRva = 0x31BA0;
static const uint8_t kMode74PublishProjPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81,
                                                     0xEC, 0x88, 0x00, 0x00, 0x00};
using Mode74PublishProj_t = void(__fastcall*)(void* self, void* edx);
Mode74PublishProj_t g_origPublishProj = nullptr;
void* g_mode74PublishProjHookAddr = nullptr;
std::atomic<bool> g_mode74PublishProjArmed{false};
std::atomic<uint32_t> g_mode74PublishProjCalls{0};
std::atomic<uint32_t> g_mode74PublishProjInjects{0};
std::atomic<uint32_t> g_mode74PubC0Writes{0};
// Live [0x17F583C] is often 0 during BuildRootA dual → Mode72 inject stalls (flat).
// Cache last known view (from live activeView or PubProj self) for src==view+0x80 match.
std::atomic<uint32_t> g_mode74CachedView{0};
std::atomic<uint32_t> g_mode74CachedViewHits{0};
std::atomic<uint32_t> g_mode74CachedViewInjects{0};
std::atomic<uint32_t> g_mode74ContentInjects{0};
// Per-eye-walk budget for content-matched view injects (avoid old ~300/frame hang).
std::atomic<uint32_t> g_mode74DualViewBudget{0};
constexpr uint32_t kMode74DualViewBudgetPerEye = 12;  // only while sparse dual gate OPEN
constexpr uint32_t kMode74TemporalViewBudget = 1;     // hitchcut/AER — match Mode72 (not 2)
// Hold HMD +0x180 for whole eye walk (HOOK used to restore before draws that re-read it).
struct Mode74HeldPub180 {
  void* self = nullptr;
  float p180[16]{};
  bool held = false;
};
constexpr int kMode74MaxHeldPub = 8;
Mode74HeldPub180 g_mode74HeldPub[kMode74MaxHeldPub]{};
int g_mode74HeldPubN = 0;
std::atomic<bool> g_mode73DualDead{false};
std::atomic<uint32_t> g_mode73DualN{0};

// Mode74 family (2026-07-25 overnight marathon):
// - 74 DEFAULT hitchcut: pair-hold temporal; dual OFF; morning-safe.
// - 75 SAFER sparse BuildRootA×2 1/8 (denser fearvr 1/2 SCRAPPED — felt LESS 3D).
// - 76 AER quality: one engine eye/frame + TextureWithPose + perfect eye matrices.
// - 77 DrawScene-only dual (Approach A — cheaper than BuildRootA×2).
// - 78 Shader-const stereo (Approach C — no second BuildRootA).
// - 79 FOV presence hammer (Approach E — prove cover FOV in log).
// Shared: HMD cover FOV, DRAW worldlook + EyeToHead/IPD, seated 6DoF, never view+0x80.
// Kill=45. Remap 73→72, 71→45. Dual never arms on 74/76/78/79.
constexpr uint32_t kMode75GateNeedLiveEs = 300;  // ~10s @30fps — safer than denser fearvr
constexpr uint32_t kMode75DualEveryN = 8;        // sparse 1/8 (denser 1/2 SCRAPPED)
constexpr uint32_t kMode74UnloadNeedEs = 8;      // activeView/cam null streak → HARD OFF
constexpr uint32_t kMode74MissCloseEs = 8;       // inject-miss while OPEN → HARD OFF
constexpr float kMode74TeleportMeters = 8.f;    // interior/exterior jump → HARD OFF
constexpr DWORD kMode75HitchMs = 45;             // ES gap ≥45ms → DUAL SESSION OFF
constexpr DWORD kMode75SoftSkipMs = 35;          // skip this dual slot if last ES slow
constexpr uint32_t kMode75SoftSkipToPause = 4;   // soft-skip streak → SESSION OFF
constexpr DWORD kMode75DualBudgetMs = 40;        // dual wall ≥ this → skip next slot
constexpr uint32_t kMode75DualBudgetSkip = 2;    // skip 2 dual slots after slow dual
constexpr float kMode74ConvergenceMeters = 2.0f;  // soft toe-in distance (parallel+toein)
constexpr uint32_t kMode78ViewBudget = 8;        // Approach C: more VIEW injects/ES
constexpr uint32_t kMode77GateNeedLiveEs = 120;  // DrawScene dual gate (~4s)
constexpr DWORD kMode77HitchMs = 50;
// Aliases for shared gate/log sites that still say Mode74 (Mode75 dual path only).
constexpr uint32_t kMode74GateNeedLiveEs = kMode75GateNeedLiveEs;
constexpr uint32_t kMode74DualEveryN = kMode75DualEveryN;
constexpr DWORD kMode74HitchMs = kMode75HitchMs;
constexpr DWORD kMode74SoftSkipMs = kMode75SoftSkipMs;
constexpr uint32_t kMode74SoftSkipToPause = kMode75SoftSkipToPause;
constexpr DWORD kMode74DualBudgetMs = kMode75DualBudgetMs;
constexpr uint32_t kMode74DualBudgetSkip = kMode75DualBudgetSkip;
std::atomic<uint32_t> g_mode77DualN{0};
std::atomic<bool> g_mode77DualDead{false};
std::atomic<uint32_t> g_mode79FovProofN{0};

// Shared pose/FOV/eye (RealVR g_RVRData / RVRGetFrameDesc analog) — cam glue + submit.
struct Mode74FrameDesc {
  bool rightEye = false;
  vr::HmdMatrix34_t pose{};
  bool poseValid = false;
  bool seatedValid = false;
  float coverTanH = 0.f;
  float coverTanV = 0.f;
  float seatedDx = 0.f, seatedDy = 0.f, seatedDz = 0.f;
  uint32_t seq = 0;
};
Mode74FrameDesc g_mode74FrameDesc{};

void Mode74SyncFrameDesc() {
  g_mode74FrameDesc.rightEye = (GetStereoEye() == StereoEye::Right);
  g_mode74FrameDesc.poseValid = GetFrameHmdPoseMatrix(&g_mode74FrameDesc.pose);
  // Freeze seated lean once per ES — do not re-sample live mid-inject (AER jump).
  float sdx = 0.f, sdy = 0.f, sdz = 0.f;
  g_mode74FrameDesc.seatedValid = GetHmdSeatedDeltaGta(&sdx, &sdy, &sdz);
  if (g_mode74FrameDesc.seatedValid) {
    static float s_prevSdx = 0.f, s_prevSdy = 0.f, s_prevSdz = 0.f;
    static bool s_haveSeat = false;
    if (s_haveSeat) {
      const float ddx = sdx - s_prevSdx;
      const float ddy = sdy - s_prevSdy;
      const float ddz = sdz - s_prevSdz;
      constexpr float kMaxSeatStep = 0.08f;  // 8 cm / frame — hitchcut clamp
      const float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
      if (d2 > kMaxSeatStep * kMaxSeatStep) {
        sdx = s_prevSdx;
        sdy = s_prevSdy;
        sdz = s_prevSdz;
      }
    }
    s_prevSdx = sdx;
    s_prevSdy = sdy;
    s_prevSdz = sdz;
    s_haveSeat = true;
    g_mode74FrameDesc.seatedDx = sdx;
    g_mode74FrameDesc.seatedDy = sdy;
    g_mode74FrameDesc.seatedDz = sdz;
  }
  float h = 0.f, v = 0.f;
  if (GetCoverFovTangents(&h, &v)) {
    g_mode74FrameDesc.coverTanH = h;
    g_mode74FrameDesc.coverTanV = v;
  }
  g_mode74FrameDesc.seq = g_frameSeq.load();
}

std::atomic<bool> g_mode74GateOpen{false};
std::atomic<bool> g_mode74DualDead{false};
std::atomic<uint32_t> g_mode74DualN{0};
std::atomic<uint32_t> g_mode74LiveStreak{0};
std::atomic<uint32_t> g_mode74MissStreak{0};
std::atomic<uint32_t> g_mode74BuildTick{0};
std::atomic<bool> g_mode74DidDualThisFrame{false};
std::atomic<uint32_t> g_mode74WorldWeakStreak{0};
std::atomic<DWORD> g_mode74LastEsMs{0};
std::atomic<uint32_t> g_mode74SoftSkipStreak{0};
std::atomic<uint32_t> g_mode74DualSkipRemain{0};  // time-budget: skip dual slots
std::atomic<DWORD> g_mode74LastDualMs{0};
uint32_t g_mode74LastInjects = 0;
// File opt-in (default OFF). Only "1" / "force" enables sparse dual.
// Session latch: after first HARD OFF, dual stays off until restart or dual=force.
std::atomic<bool> g_mode74DualOptIn{false};
std::atomic<bool> g_mode74DualEnabled{false};
std::atomic<bool> g_mode74DualSessionOff{false};

// Upload fn 0x2A1E10 — vtable MatMul×3 → +178 (OWNER-EDGE 0x2A2183 / 0x2A25FF).
// Bak ±IPD (+ soft toe-in) on this+0x80 for call duration, ALWAYS restore.
// Never leaves live view dirty (Mode71 freeze was un-restored ReplayDispatch write).
constexpr uint32_t kMode74UploadFnRva = 0x2A1E10;
static const uint8_t kMode74UploadFnPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0,
                                                   0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00};
using Mode74UploadFn_t = void(__thiscall*)(void* self, void* a0, void* a1, void* a2, void* a3,
                                           void* a4);
Mode74UploadFn_t g_origUploadFn = nullptr;
void* g_mode74UploadFnHookAddr = nullptr;
std::atomic<bool> g_mode74UploadFnArmed{false};
std::atomic<bool> g_mode74UploadFnDead{false};
std::atomic<uint32_t> g_mode74UploadFnCalls{0};
std::atomic<uint32_t> g_mode74UploadFnEyeWrites{0};
// True while uploadFn holds eye-offset in live +0x80 (Mode72 must not double-inject).
std::atomic<bool> g_mode74UploadFnOwnsEye{false};

std::atomic<uint32_t> g_mode41VsRetHits{0};
std::atomic<uint32_t> g_mode41Es{0};
uint32_t g_mode41VsTid = 0;

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
  g_holdPoseLValid = g_holdPoseRValid = false;
  g_submitPoseLValid = g_submitPoseRValid = false;
}

bool EnsureEyeRts(IDirect3DDevice9* dev, uint32_t w, uint32_t h) {
  if (!dev || w < 16 || h < 16)
    return false;
  // Device pointer changed (Reset / recreate) — old POOL_DEFAULT textures are dead.
  if (g_device && g_device != dev) {
    Log("StereoRender: device changed %p→%p — releasing eye RTs", static_cast<void*>(g_device),
        static_cast<void*>(dev));
    ReleaseEyeRts();
  }
  // Cooperative level: if lost/not-reset, skip CreateTexture (avoids hang on options).
  {
    const HRESULT coop = dev->TestCooperativeLevel();
    if (coop == D3DERR_DEVICELOST) {
      const bool had = g_texL || g_texR || g_holdL || g_holdR;
      ReleaseEyeRts();
      g_device = nullptr;
      if (had)
        Log("DeviceReset: TestCooperativeLevel DEVICELOST — released eye RTs");
      return false;
    }
    if (coop == D3DERR_DEVICENOTRESET) {
      // Reset pending — wait for HookReset; do not allocate into a dying device.
      return false;
    }
  }
  const bool mode44 = GetStereoMode() == StereoMode::FovCanvasMotionGuardRtLock ||
                      IsHeadOwnedCamFamily(GetStereoMode());
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
    Log("StereoRender: CreateTexture RT FAIL %ux%u (device may be Lost — try again after Reset)",
        w, h);
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
// Cap keeps two ARGB8 RTs (+hold pair = 4) small enough for 32-bit + FPS.
constexpr uint32_t kCanvasMaxDim = 2048;
// Mode 37 comfort: tighter cap for StretchRect/Submit cost after true-FOV fill.
constexpr uint32_t kCanvasMaxDimComfort = 1536;

void ComputeCanvasSize(uint32_t bbW, uint32_t bbH, uint32_t* outW, uint32_t* outH) {
  *outW = bbW;
  *outH = bbH;
  float coverH = 0.f, coverV = 0.f;
  const bool haveCover = GetCoverFovTangents(&coverH, &coverV);
  float gameH = 0.f, gameV = 0.f;
  GetGameFovTangents(&gameH, &gameV);

  // Mode87 DEFAULT: fixed square eye RT from gtaiv_dxvk_vr.eyert (NOT SteamVR
  // GetRecommendedRenderTargetSize). Letterbox BB into square; game still draws @ BB.
  if (UsesConfigEyeRt(GetStereoMode())) {
    static uint32_t s_lockDim = 0;
    static int s_lockMode = -1;
    if (GetStereoMode() != static_cast<StereoMode>(s_lockMode)) {
      s_lockDim = 0;
      s_lockMode = static_cast<int>(GetStereoMode());
    }
    const uint32_t dim = GetConfiguredEyeRtDim();
    if (s_lockDim == 0) {
      s_lockDim = dim;
      Log("StereoCanvasSize: Mode%d LOCKED eyeRT=%ux%u src=config-eyert bb=%ux%u "
          "(fixed square; NOT steamvr-recommended; letterbox BB→eyeRT; EyeProj ON)",
          static_cast<int>(GetStereoMode()), s_lockDim, s_lockDim, bbW, bbH);
    }
    *outW = s_lockDim;
    *outH = s_lockDim;

    static uint32_t s_loggedGen = 0xFFFFFFFFu;
    const uint32_t gen = GetGameFovPublishGeneration();
    if (gen != s_loggedGen && gen > 0) {
      s_loggedGen = gen;
      const float fillH = (haveCover && coverH > 0.05f && gameH > 0.05f) ? (gameH / coverH) : 0.f;
      const float fillV = (haveCover && coverV > 0.05f && gameV > 0.05f) ? (gameV / coverV) : 0.f;
      const float gameAspect = (gameV > 0.05f) ? (gameH / gameV) : 0.f;
      const float bbAspect = (bbH > 0) ? (static_cast<float>(bbW) / static_cast<float>(bbH)) : 0.f;
      Log("StereoCanvasSize: Mode%d eyeRT=%ux%u bb=%ux%u gameTan=(%.3f,%.3f) "
          "gameAspect=%.3f bbAspect=%.3f coverTan=(%.3f,%.3f) fill≈%.0f%%h/%.0f%%v "
          "policy=letterbox gen=%u src=config-eyert",
          static_cast<int>(GetStereoMode()), s_lockDim, s_lockDim, bbW, bbH, gameH, gameV,
          gameAspect, bbAspect, coverH, coverV, fillH * 100.f, fillV * 100.f, gen);
    }
    return;
  }

  // Mode85/86 A/B only: size Submit eye RTs from SteamVR recommended (not default).
  // Game still draws at BB; CopySurfToEyeCanvas letterboxes into this canvas.
  const bool steamVrRt = UsesSteamVrEyeRt(GetStereoMode());
  if (steamVrRt) {
    static uint32_t s_lockW = 0, s_lockH = 0;
    static int s_lockMode = -1;
    if (GetStereoMode() != static_cast<StereoMode>(s_lockMode)) {
      s_lockW = s_lockH = 0;
      s_lockMode = static_cast<int>(GetStereoMode());
    }
    uint32_t recW = 0, recH = 0;
    const bool haveRec = GetRecommendedEyeRtSize(&recW, &recH);
    uint32_t w = bbW, h = bbH;
    const char* src = "bb-fallback";
    if (haveRec) {
      w = recW;
      h = recH;
      src = "steamvr-recommended";
    }
    // Mode86: min(recommended, BB×1.25, 1440) — Mode85 2048² StretchRect hitchy.
    // Mode85: keep prior 2048 cap for A/B.
    const bool safeRt = IsSteamVrRtSafe(GetStereoMode());
    const uint32_t kSteamMaxDim = safeRt ? 1440u : 2048u;
    if (safeRt) {
      const uint32_t bbCap = (std::max)(bbW, bbH);
      const uint32_t softCap = (bbCap > 0) ? (bbCap + bbCap / 4u) : kSteamMaxDim;  // ×1.25
      if (w > softCap)
        w = softCap;
      if (h > softCap)
        h = softCap;
    }
    if (w > kSteamMaxDim)
      w = kSteamMaxDim;
    if (h > kSteamMaxDim)
      h = kSteamMaxDim;
    if (s_lockW == 0) {
      s_lockW = w;
      s_lockH = h;
      Log("StereoCanvasSize: Mode%d LOCKED eyeRT=%ux%u src=%s recommended=%ux%u bb=%ux%u "
          "maxDim=%u (letterbox BB→SteamVR RT; policy=letterbox; A/B only)%s",
          static_cast<int>(GetStereoMode()), s_lockW, s_lockH, src, recW, recH, bbW, bbH,
          kSteamMaxDim, safeRt ? " noMidDrawEyeProj dual1/2" : "");
    }
    w = s_lockW;
    h = s_lockH;
    *outW = w;
    *outH = h;

    static uint32_t s_loggedGen = 0xFFFFFFFFu;
    const uint32_t gen = GetGameFovPublishGeneration();
    if (gen != s_loggedGen && gen > 0) {
      s_loggedGen = gen;
      const float fillH = (haveCover && coverH > 0.05f && gameH > 0.05f) ? (gameH / coverH) : 0.f;
      const float fillV = (haveCover && coverV > 0.05f && gameV > 0.05f) ? (gameV / coverV) : 0.f;
      const float gameAspect = (gameV > 0.05f) ? (gameH / gameV) : 0.f;
      const float bbAspect = (bbH > 0) ? (static_cast<float>(bbW) / static_cast<float>(bbH)) : 0.f;
      Log("StereoCanvasSize: Mode%d recommended=%ux%u eyeRT=%ux%u bb=%ux%u gameTan=(%.3f,%.3f) "
          "gameAspect=%.3f bbAspect=%.3f coverTan=(%.3f,%.3f) fill≈%.0f%%h/%.0f%%v "
          "policy=letterbox gen=%u src=%s",
          static_cast<int>(GetStereoMode()), recW, recH, w, h, bbW, bbH, gameH, gameV,
          gameAspect, bbAspect, coverH, coverV, fillH * 100.f, fillV * 100.f, gen, src);
    }
    return;
  }

  if (!haveCover)
    return;
  if (!(gameH > 0.05f) || !(gameV > 0.05f))
    return;

  float sx = coverH / gameH;
  float sy = coverV / gameV;
  sx = (std::max)(1.f, (std::min)(sx, 4.f));
  sy = (std::max)(1.f, (std::min)(sy, 4.f));

  uint32_t w = static_cast<uint32_t>(bbW * sx + 0.5f);
  uint32_t h = static_cast<uint32_t>(bbH * sy + 0.5f);
  const bool comfort = UsesFovComfortPath(GetStereoMode());
  const uint32_t maxDim = comfort ? kCanvasMaxDimComfort : kCanvasMaxDim;
  if (w > maxDim)
    w = maxDim;
  if (h > maxDim)
    h = maxDim;

  // Mode 37: lock canvas size after first true-FOV sample. FOV wobble was
  // recreating 4 RTs (1536↔1440) every few frames = jump + FPS hit.
  static uint32_t s_lockW = 0, s_lockH = 0;
  static int s_lockMode = -1;
  if (!comfort || GetStereoMode() != static_cast<StereoMode>(s_lockMode)) {
    s_lockW = s_lockH = 0;
    s_lockMode = static_cast<int>(GetStereoMode());
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
    const float gameAspect = (gameV > 0.05f) ? (gameH / gameV) : 0.f;
    const float bbAspect = (bbH > 0) ? (static_cast<float>(bbW) / static_cast<float>(bbH)) : 0.f;
    const char* polStr = "letterbox";
    if (GetCanvasAspectPolicy(GetStereoMode()) == CanvasAspectPolicy::FillPrefer)
      polStr = "fill";
    else if (GetCanvasAspectPolicy(GetStereoMode()) == CanvasAspectPolicy::SoftLetterbox)
      polStr = IsCullSyncPresence(GetStereoMode()) ? "soft68" : "soft78";
    Log("StereoCanvasSize: bb=%ux%u canvas=%ux%u sx=%.2f sy=%.2f gameTan=(%.3f,%.3f) "
        "gameAspect=%.3f bbAspect=%.3f coverTan=(%.3f,%.3f) fill≈%.0f%%h/%.0f%%v "
        "trueFov=%d gen=%u maxDim=%u locked=%d policy=%s",
        bbW, bbH, w, h, sx, sy, gameH, gameV, gameAspect, bbAspect, coverH, coverV,
        fillH * 100.f, fillV * 100.f, IsGameFovFromCCamActive() ? 1 : 0, gen, maxDim,
        (comfort && s_lockW) ? 1 : 0, polStr);
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

  bool ok = false;
  if (rc.right - rc.left >= 16 && rc.bottom - rc.top >= 16 && src.right - src.left >= 16 &&
      src.bottom - src.top >= 16) {
    // Letterbox needs black bars. Mode88 hitch cut: ColorFill only when dest rect
    // does not cover the full canvas (otherwise StretchRect overwrites all).
    const bool fullCover =
        rc.left <= 0 && rc.top <= 0 &&
        rc.right >= static_cast<LONG>(dd.Width) && rc.bottom >= static_cast<LONG>(dd.Height);
    if (!fullCover)
      dev->ColorFill(dst, nullptr, D3DCOLOR_XRGB(0, 0, 0));
    // Mode88 hitch cut: POINT filter (LINEAR StretchRect cost showed up on streets).
    const D3DTEXTUREFILTERTYPE filt =
        IsHitchCutEyeProj(GetStereoMode()) ? D3DTEXF_NONE : D3DTEXF_LINEAR;
    ok = SUCCEEDED(dev->StretchRect(bb, &src, dst, &rc, filt));
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
  static LARGE_INTEGER s_qpf{};
  if (s_qpf.QuadPart == 0)
    QueryPerformanceFrequency(&s_qpf);
  LARGE_INTEGER t0{}, t1{};
  QueryPerformanceCounter(&t0);
  const bool ok = CopySurfToEyeCanvas(dev, bb, tex, eye);
  QueryPerformanceCounter(&t1);
  bb->Release();
  const double ms =
      (s_qpf.QuadPart > 0)
          ? (1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
             static_cast<double>(s_qpf.QuadPart))
          : 0.0;
  // StretchRect cost profile (street hitch): log slow copies + rolling average.
  static std::atomic<uint32_t> s_n{0};
  static std::atomic<uint32_t> s_sumUs{0};  // microseconds sum
  static std::atomic<uint32_t> s_slow{0};
  ++s_n;
  const uint32_t us = static_cast<uint32_t>(ms * 1000.0 + 0.5);
  s_sumUs.fetch_add(us);
  if (ms >= 2.0)
    ++s_slow;
  const uint32_t n = s_n.load();
  if (n == 1 || (n % 400) == 0 || ms >= 4.0) {
    const uint32_t sumUs = s_sumUs.load();
    const float avgMs = n > 0 ? (static_cast<float>(sumUs) / 1000.f / static_cast<float>(n)) : 0.f;
    Log("StretchRect: copy ms=%.2f avg=%.2f n=%u slow2ms+=%u eyeRT=%ux%u (street hitch probe)",
        ms, avgMs, n, s_slow.load(), g_rtW, g_rtH);
  }
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

bool Mode72ReadActiveView(uint32_t* outActive);
uint32_t Mode74ResolveViewForInject(bool* usedCache);
void Mode74BeginEyePubHold();
void Mode74RestoreHeldPub180();
bool Mode74HoldPub180(void* self, const float* backup180);
void Mode74BeginHardOff(const char* reason);
bool Mode71MatSaneForInject(const float m[16]);
bool Mode72TryCopySrc(const float* src, float out[16]);
bool Mode74ApplyHmdEyeLocal(float* m);
bool Mode74LooksLikeProjection(const float* m);

// Row-major 4×4: out = A * B (same convention as game MatMul 0x307F0).
void Mode74Mat4Mul(float out[16], const float a[16], const float b[16]) {
  float tmp[16]{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      tmp[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                       a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
    }
  }
  for (int i = 0; i < 16; ++i)
    out[i] = tmp[i];
}

// Cache per-eye HMD projection once per dual (L≠R asymmetric; shared near/far defaults).
// Optional template from activeView+0x180 (PublishSync proj slot / PublishProj input).
void Mode74CacheEyeProjections() {
  g_mode74ProjCacheOk.store(false);
  float tmpl[16]{};
  const float* tmplPtr = nullptr;
  uint32_t active = 0;
  if (Mode72ReadActiveView(&active) && active) {
    __try {
      const auto* p =
          reinterpret_cast<const float*>(static_cast<uintptr_t>(active) + 0x180);
      for (int i = 0; i < 16; ++i)
        tmpl[i] = p[i];
      tmplPtr = tmpl;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      tmplPtr = nullptr;
    }
  }
  const bool okL = BuildHmdEyeProjection(vr::Eye_Left, g_mode74CachedProjL, tmplPtr);
  const bool okR = BuildHmdEyeProjection(vr::Eye_Right, g_mode74CachedProjR, tmplPtr);
  g_mode74ProjCacheOk.store(okL && okR);
  static uint32_t s_projCacheLog = 0;
  if ((++s_projCacheLog) <= 3 || (s_projCacheLog % 600) == 0) {
    float l = 0.f, r = 0.f, t = 0.f, b = 0.f;
    GetEyeRawProjection(vr::Eye_Left, &l, &r, &t, &b);
    Log("EyeProj: cache ok=%d tmpl=%d Lraw=(%.3f,%.3f,%.3f,%.3f) Lsx=%.3f ox=%.3f "
        "(asymmetric HMD; never view+0x80)",
        (okL && okR) ? 1 : 0, tmplPtr ? 1 : 0, l, r, t, b, g_mode74CachedProjL[0],
        g_mode74CachedProjL[8]);
  }
}

bool Mode74GetCachedEyeProj(float out[16]) {
  if (!g_mode74ProjCacheOk.load() || !out)
    return false;
  const float* src =
      (GetStereoEye() == StereoEye::Right) ? g_mode74CachedProjR : g_mode74CachedProjL;
  for (int i = 0; i < 16; ++i)
    out[i] = src[i];
  return true;
}

struct Mode74ProjBackup {
  float p180[16]{};
  float pC0[16]{};
  float p308[24]{};  // 6×4 frustum publish block
  uint32_t viewAddr = 0;
  bool have180 = false;
  bool haveC0 = false;
  bool have308 = false;
};

// Build ±half eye-offset LOCAL copy of a view matrix (never writes live +0x80).
// Soft toe-in: yaw eyes toward a convergence point (~2m) — parallel-only feels "window".
// Also applies EyeToHead forward (OpenVR m[2][3]) like CamMatrix / L4D2 m_EyeZ.
void Mode74OffsetViewLocal(float viewEye[16], bool rightEye) {
  // Honor user IPD (F8): 1cm = most fused. NEVER floor half to 5cm (was forcing ~10cm
  // sep and making lowest F8 look least fused / inverted).
  float half = 0.5f * GetStereoSepMeters() * GetStereoScale();
  if (half < 0.f)
    half = 0.f;
  if (half > kMode71MaxHalfSep)
    half = kMode71MaxHalfSep;
  const float sign = rightEye ? 1.f : -1.f;
  const float rlen = std::sqrt(viewEye[0] * viewEye[0] + viewEye[1] * viewEye[1] +
                               viewEye[2] * viewEye[2]);
  const float inv = (rlen > 1e-3f) ? (1.f / rlen) : 0.f;
  viewEye[12] += sign * half * viewEye[0] * inv;
  viewEye[13] += sign * half * viewEye[1] * inv;
  viewEye[14] += sign * half * viewEye[2] * inv;

  // EyeToHead forward offset (OpenVR +Z back → GTA forward = -eyeZ along view forward).
  float eyeZ = 0.f;
  vr::HmdMatrix34_t e2h{};
  if (GetCachedEyeToHead(rightEye, &e2h))
    eyeZ = e2h.m[2][3];
  if (std::fabs(eyeZ) > 1e-5f && std::fabs(eyeZ) < 0.2f) {
    const float flen = std::sqrt(viewEye[4] * viewEye[4] + viewEye[5] * viewEye[5] +
                                 viewEye[6] * viewEye[6]);
    if (flen > 1e-3f) {
      const float finv = 1.f / flen;
      const float fzOff = -eyeZ;
      viewEye[12] += fzOff * viewEye[4] * finv;
      viewEye[13] += fzOff * viewEye[5] * finv;
      viewEye[14] += fzOff * viewEye[6] * finv;
    }
  }

  // Toe-in: rotate right/forward in the horizontal plane by ±atan(half/convergence).
  // GTA view: row0=right, row1=forward (CamMatrix convention for uploaded view).
  const float conv = kMode74ConvergenceMeters;
  if (conv > 0.25f && half > 1e-4f) {
    const float ang = std::atan(half / conv);  // ~1.4° at 5cm/2m
    const float ca = std::cos(ang);
    const float sa = std::sin(ang) * (-sign);  // L yaws right (+), R yaws left (−) toward center
    const float rx = viewEye[0], ry = viewEye[1], rz = viewEye[2];
    const float fx = viewEye[4], fy = viewEye[5], fz = viewEye[6];
    viewEye[0] = rx * ca + fx * sa;
    viewEye[1] = ry * ca + fy * sa;
    viewEye[2] = rz * ca + fz * sa;
    viewEye[4] = fx * ca - rx * sa;
    viewEye[5] = fy * ca - ry * sa;
    viewEye[6] = fz * ca - rz * sa;
  }
}

void Mode74PushDeviceViewSrcCopy(const float viewEye[16]) {
  if (!g_device || !viewEye || !Mode71MatSaneForInject(viewEye))
    return;
  // Prefer D3DTS_VIEW (safe from build thread). Raw SetVSConstF from BuildRootA
  // faulted Mode74 → 45 on 20260725-1505.
  __try {
    g_device->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(viewEye));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    static uint32_t s_xfExc = 0;
    if ((++s_xfExc) <= 4)
      Log("Mode74: D3DTS_VIEW push EXCEPTION n=%u (ignored; dual continues)", s_xfExc);
    return;
  }
  static uint32_t s_pushN = 0;
  const uint32_t pn = ++s_pushN;
  if (pn <= 4 || (pn % 300) == 0) {
    Log("Mode74: device VIEW push #%u eye=%s t=(%.2f,%.2f,%.2f) "
        "(Rage HMD+IPD src-copy; never view+0x80; never build-thread SetVSConstF)",
        pn, (GetStereoEye() == StereoEye::Right) ? "R" : "L", viewEye[12], viewEye[13],
        viewEye[14]);
  }
}

// Per-eye walk: HMD proj → +0x180, VP → +0xC0 (=P * eye-offset view copy).
// Never touches view+0x80 (Mode71 freeze path). Restore after BuildRootA.
bool Mode74WritePubProjSlots(Mode74ProjBackup* bak) {
  if (!bak || !g_mode74ProjCacheOk.load())
    return false;
  bool usedCache = false;
  uint32_t active = Mode74ResolveViewForInject(&usedCache);
  if (!active) {
    static uint32_t s_noActive = 0;
    if ((++s_noActive) <= 4 || (s_noActive % 600) == 0)
      Log("PubProj: WRITE miss no-view (live+cache) n=%u", s_noActive);
    return false;
  }
  bak->viewAddr = active;
  __try {
    auto* base = reinterpret_cast<float*>(static_cast<uintptr_t>(active));
    auto* p180 = base + (0x180 / 4);
    auto* pC0 = base + (0xC0 / 4);
    auto* p80 = base + (0x80 / 4);
    auto* p308 = base + (0x308 / 4);

    for (int i = 0; i < 16; ++i)
      bak->p180[i] = p180[i];
    bak->have180 = true;
    for (int i = 0; i < 16; ++i)
      bak->pC0[i] = pC0[i];
    bak->haveC0 = true;
    for (int i = 0; i < 24; ++i)
      bak->p308[i] = p308[i];
    bak->have308 = true;

    float proj[16]{};
    const vr::EVREye eye =
        (GetStereoEye() == StereoEye::Right) ? vr::Eye_Right : vr::Eye_Left;
    // Prefer game template when it looks like proj; else fresh HMD matrix.
    const float* tmpl = Mode74LooksLikeProjection(bak->p180) ? bak->p180 : nullptr;
    if (!BuildHmdEyeProjection(eye, proj, tmpl)) {
      static uint32_t s_buildFail = 0;
      if ((++s_buildFail) <= 4 || (s_buildFail % 600) == 0)
        Log("PubProj: WRITE miss BuildHmdEyeProjection fail n=%u", s_buildFail);
      return false;
    }
    for (int i = 0; i < 16; ++i)
      p180[i] = proj[i];

    // Eye-offset LOCAL copy of view (+0x80) for VP — never write live view+0x80.
    // MUST ApplyHmdEyeLocal first (worldlook). Offset-only = tilted cinema / no tracking
    // on Mode75 dual (headset 2026-07-25 HARD FAIL).
    float viewEye[16]{};
    for (int i = 0; i < 16; ++i)
      viewEye[i] = p80[i];
    Mode74ApplyHmdEyeLocal(viewEye);
    Mode74OffsetViewLocal(viewEye, eye == vr::Eye_Right);
    // half already applied inside OffsetViewLocal — no second floor.

    // PublishSync: [this+0xC0] = MatMul([this+0x180], viewEye) = P * ViewEye.
    Mode74Mat4Mul(pC0, proj, viewEye);
    const uint32_t nC0 = ++g_mode74PubC0Writes;

    Mode74PushDeviceViewSrcCopy(viewEye);

    bool ranProj = false;
    const uint32_t n = ++g_mode74PublishProjInjects;
    if (g_origPublishProj) {
      g_origPublishProj(reinterpret_cast<void*>(static_cast<uintptr_t>(active)), nullptr);
      ranProj = true;
    }

    // Mode88/89: quieter (street hitch from log I/O); else every 300.
    const uint32_t every =
        (IsHitchCutEyeProj(GetStereoMode()) || IsCullSyncPresence(GetStereoMode())) ? 2400u
                                                                                    : 300u;
    if (n <= 4 || (n % every) == 0)
      Log("PubProj: WRITE eye=%s +0x180 sx=%.3f ox=%.3f +0xC0=#%u sep=%.1fcm "
          "cache=%d callProj=%d view=0x%X (VP from eye-offset view copy; never view+0x80)",
          (eye == vr::Eye_Right) ? "R" : "L", proj[0], proj[8], nC0,
          GetStereoSepMeters() * 100.f, usedCache ? 1 : 0, ranProj ? 1 : 0, active);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    static uint32_t s_exc = 0;
    if ((++s_exc) <= 4 || (s_exc % 600) == 0)
      Log("PubProj: WRITE miss EXCEPTION n=%u view was set", s_exc);
    return false;
  }
}

void Mode74RestorePubProjSlots(const Mode74ProjBackup& bak) {
  uint32_t active = bak.viewAddr;
  if (!active) {
    bool usedCache = false;
    active = Mode74ResolveViewForInject(&usedCache);
  }
  if (!active)
    return;
  __try {
    auto* base = reinterpret_cast<float*>(static_cast<uintptr_t>(active));
    if (bak.have180) {
      auto* p180 = base + (0x180 / 4);
      for (int i = 0; i < 16; ++i)
        p180[i] = bak.p180[i];
    }
    if (bak.haveC0) {
      auto* pC0 = base + (0xC0 / 4);
      for (int i = 0; i < 16; ++i)
        pC0[i] = bak.pC0[i];
    }
    if (bak.have308) {
      auto* p308 = base + (0x308 / 4);
      for (int i = 0; i < 24; ++i)
        p308[i] = bak.p308[i];
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

bool Mode74ViewNearLiveCam(const float* vm) {
  if (!vm || !Mode71MatSaneForInject(vm))
    return false;
  float cx = 0.f, cy = 0.f, cz = 0.f;
  if (!GetLastStereoCamPos(&cx, &cy, &cz))
    return true;  // no cam yet — accept first sane
  // World-matrix style (Rage cam): translation ≈ eye position.
  float dx = vm[12] - cx, dy = vm[13] - cy, dz = vm[14] - cz;
  if ((dx * dx + dy * dy + dz * dz) < (40.f * 40.f))
    return true;
  // View-matrix style (D3D): eye = -R^T * t
  const float ex = -(vm[0] * vm[12] + vm[4] * vm[13] + vm[8] * vm[14]);
  const float ey = -(vm[1] * vm[12] + vm[5] * vm[13] + vm[9] * vm[14]);
  const float ez = -(vm[2] * vm[12] + vm[6] * vm[13] + vm[10] * vm[14]);
  dx = ex - cx;
  dy = ey - cy;
  dz = ez - cz;
  return (dx * dx + dy * dy + dz * dz) < (40.f * 40.f);
}

bool Mode74TryReadViewMat80(void* self, float out[16]) {
  if (!self || !out)
    return false;
  __try {
    const auto* p80 =
        reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(self) + 0x80);
    for (int i = 0; i < 16; ++i)
      out[i] = p80[i];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// PublishProj 0x31BA0: stamp HMD +0x180 → rebuild +0x308 (drawn frustum FOV).
// Dual: HOLD +0x180 until end of eye BuildRootA.
// Temporal (dualEn=0): HOLD +0x180 once per self/ES (restore on EndScene) — streets
// used to stamp ~20×/ES and thrash FOV I/O; apartment was smooth (fewer draws).
void __fastcall HookMode74PublishProj(void* self, void* edx) {
  ++g_mode74PublishProjCalls;
  if (!g_origPublishProj)
    return;
  const bool mode74Fam = IsMode74Family(GetStereoMode()) && self;
  if (!mode74Fam) {
    g_origPublishProj(self, edx);
    return;
  }
  const bool dual = g_inDual.load();
  __try {
    const uint32_t selfU = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint32_t active = 0;
    const bool isActive =
        Mode72ReadActiveView(&active) && active != 0 && active == selfU;
    const uint32_t cached = g_mode74CachedView.load();
    const bool isCached = cached != 0 && cached == selfU;

    // Temporal path hitch cut: skip lights/UI; HOLD HMD +0x180 once per self/ES.
    if (!dual) {
      if (!isActive && !isCached) {
        static uint32_t s_probe = 0;
        if ((++s_probe % 96) != 0) {
          g_origPublishProj(self, edx);
          return;
        }
        float vm[16]{};
        if (!(Mode74TryReadViewMat80(self, vm) && Mode74ViewNearLiveCam(vm))) {
          g_origPublishProj(self, edx);
          return;
        }
      }
      if (selfU)
        g_mode74CachedView.store(selfU);

      // Already holding this self → orig only (HMD stays in +0x180 for +0x308 rebuild).
      for (int i = 0; i < g_mode74HeldPubN; ++i) {
        if (g_mode74HeldPub[i].held && g_mode74HeldPub[i].self == self) {
          g_origPublishProj(self, edx);
          return;
        }
      }

      auto* p180 = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(self) + 0x180);
      float backup[16]{};
      for (int i = 0; i < 16; ++i)
        backup[i] = p180[i];
      float proj[16]{};
      if (!Mode74GetCachedEyeProj(proj)) {
        const vr::EVREye eye =
            (GetStereoEye() == StereoEye::Right) ? vr::Eye_Right : vr::Eye_Left;
        const float* tmpl = Mode74LooksLikeProjection(backup) ? backup : nullptr;
        if (!BuildHmdEyeProjection(eye, proj, tmpl)) {
          g_origPublishProj(self, edx);
          return;
        }
      }
      Mode74HoldPub180(self, backup);
      for (int i = 0; i < 16; ++i)
        p180[i] = proj[i];
      g_origPublishProj(self, edx);
      const uint32_t n = ++g_mode74PublishProjInjects;
      const uint32_t every =
          (IsHitchCutEyeProj(GetStereoMode()) || IsCullSyncPresence(GetStereoMode()))
              ? 4800u
              : 1200u;
      if (n <= 4 || (n % every) == 0)
        Log("PubProj: TEMPORAL HOLD sx=%.3f ox=%.3f n=%u act=%d cache=%d "
            "(1×/self/ES; restore EndScene; never view+0x80; dualEn=0)",
            proj[0], proj[8], n, isActive ? 1 : 0, isCached ? 1 : 0);
      return;
    }

    // Dual path (opt-in only): nearCam check + HOLD +0x180 + optional view sep.
    float vm[16]{};
    const bool haveVm = Mode74TryReadViewMat80(self, vm);
    const bool nearCam = haveVm && Mode74ViewNearLiveCam(vm);
    if (selfU && (nearCam || isActive))
      g_mode74CachedView.store(selfU);
    if (!g_mode74ProjCacheOk.load()) {
      g_origPublishProj(self, edx);
      return;
    }
    auto* p180 = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(self) + 0x180);
    float backup[16]{};
    for (int i = 0; i < 16; ++i)
      backup[i] = p180[i];
    float proj[16]{};
    if (!Mode74GetCachedEyeProj(proj)) {
      const vr::EVREye eye =
          (GetStereoEye() == StereoEye::Right) ? vr::Eye_Right : vr::Eye_Left;
      const float* tmpl = Mode74LooksLikeProjection(backup) ? backup : nullptr;
      if (!BuildHmdEyeProjection(eye, proj, tmpl)) {
        g_origPublishProj(self, edx);
        return;
      }
    }
    const vr::EVREye eye =
        (GetStereoEye() == StereoEye::Right) ? vr::Eye_Right : vr::Eye_Left;
    Mode74HoldPub180(self, backup);
    for (int i = 0; i < 16; ++i)
      p180[i] = proj[i];
    if (nearCam) {
      float viewEye[16]{};
      for (int i = 0; i < 16; ++i)
        viewEye[i] = vm[i];
      // Same as SetVSConstF path: worldlook THEN ±IPD (Mode75 lost tracking without this).
      Mode74ApplyHmdEyeLocal(viewEye);
      Mode74OffsetViewLocal(viewEye, eye == vr::Eye_Right);
      __try {
        auto* pC0 = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(self) + 0xC0);
        Mode74Mat4Mul(pC0, proj, viewEye);
        ++g_mode74PubC0Writes;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
      Mode74PushDeviceViewSrcCopy(viewEye);
    }
    g_origPublishProj(self, edx);
    const uint32_t n = ++g_mode74PublishProjInjects;
    const uint32_t every =
        (IsHitchCutEyeProj(GetStereoMode()) || IsCullSyncPresence(GetStereoMode())) ? 2400u
                                                                                    : 600u;
    if (n <= 4 || (n % every) == 0)
      Log("PubProj: HOOK eye=%s sx=%.3f ox=%.3f n=%u nearCam=%d "
          "(0x31BA0 → +0x308; HOLD +0x180; cacheView=0x%X; never view+0x80)",
          (eye == vr::Eye_Right) ? "R" : "L", proj[0], proj[8], n, nearCam ? 1 : 0, selfU);
    return;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  g_origPublishProj(self, edx);
}

bool Mode74InstallPublishProjHook();  // defined near Mode72InstallHook
bool Mode74InstallUploadFnHook();

// Upload fn 0x2A1E10: thiscall + 5 stack args (retn 0x14). Bak±IPD on +0x80, call, restore.
static void __stdcall Mode74UploadFnImpl(void* self, void* a0, void* a1, void* a2, void* a3,
                                         void* a4) {
  float bak[16]{};
  float* p80 = nullptr;
  bool did = false;
  const StereoMode sm = GetStereoMode();
  const bool want =
      IsMode74Family(sm) && g_mode74UploadFnArmed.load() &&
      !g_mode74UploadFnDead.load() && self && g_origUploadFn;
  if (want) {
    __try {
      p80 = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(self) + 0x80);
      if (Mode71MatSaneForInject(p80) && Mode74ViewNearLiveCam(p80)) {
        for (int i = 0; i < 16; ++i)
          bak[i] = p80[i];
        // OwnsEye skips SetVSConstF inject — MUST stamp HMD look here or Mode74/75
        // fall back to body/follow basis (jump / no tracking). Restore bak after.
        Mode74ApplyHmdEyeLocal(p80);
        Mode74OffsetViewLocal(p80, GetStereoEye() == StereoEye::Right);
        did = true;
        g_mode74UploadFnOwnsEye.store(true);
        const uint32_t nw = ++g_mode74UploadFnEyeWrites;
        if (nw <= 6 || (nw % 200) == 0) {
          const float dx = p80[12] - bak[12];
          const float dy = p80[13] - bak[13];
          const float dz = p80[14] - bak[14];
          const float sep = std::sqrt(dx * dx + dy * dy + dz * dz);
          Log("UploadFn: EYE write #%u eye=%s self=0x%X eyeSep=%.1fcm hmdLook+toeIn=1 "
              "(bak+0x80 for MatMul×3/+178; ALWAYS restore; never leave dirty)",
              nw, (GetStereoEye() == StereoEye::Right) ? "R" : "L",
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)), sep * 100.f);
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      did = false;
      p80 = nullptr;
      g_mode74UploadFnOwnsEye.store(false);
    }
  }
  ++g_mode74UploadFnCalls;
  __try {
    if (g_origUploadFn)
      g_origUploadFn(self, a0, a1, a2, a3, a4);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    static uint32_t s_exc = 0;
    if ((++s_exc) <= 4)
      Log("UploadFn: EXCEPTION in orig n=%u — disabling uploadFn eye writes", s_exc);
    g_mode74UploadFnDead.store(true);
  }
  if (did && p80) {
    __try {
      for (int i = 0; i < 16; ++i)
        p80[i] = bak[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      static uint32_t s_restExc = 0;
      if ((++s_restExc) <= 4)
        Log("UploadFn: EXCEPTION restoring +0x80 n=%u — kill risk; dual stays OFF", s_restExc);
      g_mode74UploadFnDead.store(true);
    }
  }
  g_mode74UploadFnOwnsEye.store(false);
}

#if defined(_MSC_VER) && defined(_M_IX86)
static void __declspec(naked) HookMode74UploadFnNaked() {
  __asm {
    push ebp
    mov ebp, esp
    push dword ptr [ebp+0x18]
    push dword ptr [ebp+0x14]
    push dword ptr [ebp+0x10]
    push dword ptr [ebp+0xC]
    push dword ptr [ebp+8]
    push ecx
    call Mode74UploadFnImpl
    pop ebp
    ret 0x14
  }
}
#endif

bool Mode74LooksLikeProjection(const float* m) {
  if (!m)
    return false;
  if (!(m[0] > 0.1f && m[0] < 8.f && m[5] > 0.1f && m[5] < 8.f))
    return false;
  if (std::fabs(m[15]) > 0.08f)
    return false;
  if (std::fabs(m[11]) < 0.85f || std::fabs(m[11]) > 1.15f)
    return false;
  // Reject view-like (unit right + large translation).
  const float rlen = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  const float t2 = m[12] * m[12] + m[13] * m[13] + m[14] * m[14];
  if (rlen > 0.85f && rlen < 1.15f && t2 > 1.f)
    return false;
  return true;
}

// Mode 73/75: same-frame world×2 via BuildRootA (safer sparse on 75; denser fearvr SCRAPPED).
// Us: freeze pose → L/R SetStereoEye + PubProj HMD look/IPD/proj → BuildRootA×2
// → canvas capture → restore eye. Never mutates live view+0x80.
bool RunMode73SameFrameDualGuarded(void* self, void* edx) {
  __try {
    // fear-vr: one render request / frame. Freeze HMD+seated for both eyes.
    BeginFrameHmdPoseSample();
    Mode74SyncFrameDesc();
    Mode74CacheEyeProjections();  // asymmetric L/R HMD proj (fear-vr SharedSymmetricFov class)

    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();  // CamMatrix IPD on during dual (DRAW owns outside)
    Mode74BeginEyePubHold();
    Mode74ProjBackup bakL{};
    Mode74WritePubProjSlots(&bakL);  // HMD look + ±IPD + eye proj; restore after walk
    // Never PushLiveCamEyeView — D3D-layout overwrite → ~70° cinema (Mode75 FAIL).
    g_origRoot[1](self, edx);
    Mode74RestoreHeldPub180();
    Mode74RestorePubProjSlots(bakL);
    if (g_device && g_texL && CopyBbToEyeCanvas(g_device, g_texL, vr::Eye_Left)) {
      g_haveL = true;
      SnapshotHoldPose(false);
      g_submitPoseL = g_holdPoseL;
      g_submitPoseLValid = g_holdPoseLValid;
    }

    SetStereoEye(StereoEye::Right);
    RefreshLiveCamForStereoEye();
    Mode74BeginEyePubHold();
    Mode74ProjBackup bakR{};
    Mode74WritePubProjSlots(&bakR);
    g_origRoot[1](self, edx);
    Mode74RestoreHeldPub180();
    Mode74RestorePubProjSlots(bakR);
    if (g_device && g_texR && CopyBbToEyeCanvas(g_device, g_texR, vr::Eye_Right)) {
      g_haveR = true;
      SnapshotHoldPose(true);
      g_submitPoseR = g_holdPoseR;
      g_submitPoseRValid = g_holdPoseRValid;
    }

    // fear-vr: restore camera after both eyes.
    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    return false;
  }
}

// Mode 77 (Approach A): cheaper same-frame — dual DrawScene BuildRenderList ONLY.
// PhaseA/C already ran once; we rebuild world draw list L then R with Mode74 eye
// matrices. Never full BuildRootA×2. Never live view+0x80.
bool RunMode77DrawSceneDualGuarded(void* drawSelf, void* edx) {
  if (!g_origBuild || !drawSelf)
    return false;
  __try {
    BeginFrameHmdPoseSample();
    Mode74SyncFrameDesc();
    Mode74CacheEyeProjections();

    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    Mode74BeginEyePubHold();
    Mode74ProjBackup bakL{};
    Mode74WritePubProjSlots(&bakL);
    g_origBuild(drawSelf, edx);
    Mode74RestoreHeldPub180();
    Mode74RestorePubProjSlots(bakL);
    if (g_device && g_texL && CopyBbToEyeCanvas(g_device, g_texL, vr::Eye_Left)) {
      g_haveL = true;
      SnapshotHoldPose(false);
      g_submitPoseL = g_holdPoseL;
      g_submitPoseLValid = g_holdPoseLValid;
    }

    SetStereoEye(StereoEye::Right);
    RefreshLiveCamForStereoEye();
    Mode74BeginEyePubHold();
    Mode74ProjBackup bakR{};
    Mode74WritePubProjSlots(&bakR);
    g_origBuild(drawSelf, edx);
    Mode74RestoreHeldPub180();
    Mode74RestorePubProjSlots(bakR);
    if (g_device && g_texR && CopyBbToEyeCanvas(g_device, g_texR, vr::Eye_Right)) {
      g_haveR = true;
      SnapshotHoldPose(true);
      g_submitPoseR = g_holdPoseR;
      g_submitPoseRValid = g_holdPoseRValid;
    }

    SetStereoEye(StereoEye::Left);
    RefreshLiveCamForStereoEye();
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    return false;
  }
}

bool Mode74ShouldDual() {
  // Dual only on Mode 75 (separate stereo mode). Never on 74 hitchcut or 76 AER.
  return IsSparseSessionDual(GetStereoMode()) && g_mode74DualEnabled.load() &&
         g_mode74GateOpen.load() && !g_mode74DualDead.load() &&
         !g_mode74DualSessionOff.load() && g_mode72Armed.load() && !g_mode72Dead.load();
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

  // Mode 73: ungated BuildRootA×2 (DISABLED via RemapLoadedStereoMode → 72).
  if (mode == StereoMode::SameFrameVsConstDual && !g_mode73DualDead.load() && !g_inDual.load() &&
      IsCamMatrixOverrideEnabled() && g_device && g_texL && g_texR && g_mode72Armed.load()) {
    g_inDual.store(true);
    const bool ok = RunMode73SameFrameDualGuarded(self, edx);
    g_inDual.store(false);
    if (!ok) {
      g_mode73DualDead.store(true);
      g_haveL = g_haveR = false;
      Log("Mode73: EXCEPTION in same-frame dual — permanently disabled; mono BuildRootA; "
          "wrote stereo=45");
      WriteStereoModeFile(45);
      g_origRoot[1](self, edx);
      return;
    }
    const uint32_t n = ++g_mode73DualN;
    if (n <= 6 || (n % 120) == 0)
      Log("Mode73: SAME-FRAME dual #%u haveL=%d haveR=%d injects=%u sep=%.0fcm "
          "(BuildRootA×2 + VSConst src-copy)",
          n, g_haveL ? 1 : 0, g_haveR ? 1 : 0, g_mode72Injects.load(),
          GetStereoSepMeters() * 100.f);
    if (g_haveL && g_haveR && (n == 5 || (n % 300) == 0))
      CompareEyeCanvases(g_device);
    return;
  }

  // Mode 75: SAFER sparse same-frame dual after streaming gate; every 1/8.
  // Denser fearvr 1/2 SCRAPPED (headset: LESS 3D). Off-tick / soft-skip /
  // budget-skip: mono BuildRootA + HOLD last true L/R pair.
  // Note: tick%1 is always 0 in C++, so EveryN<=1 must force dualSlot.
  if (Mode74ShouldDual() && !g_inDual.load() && IsCamMatrixOverrideEnabled() && g_device &&
      g_texL && g_texR) {
    const uint32_t tick = g_mode74BuildTick.fetch_add(1) + 1;
    const bool dualSlot =
        (kMode75DualEveryN <= 1) || ((tick % kMode75DualEveryN) == 1);
    if (dualSlot) {
      const uint32_t budgetSkip = g_mode74DualSkipRemain.load();
      if (budgetSkip > 0) {
        g_mode74DualSkipRemain.store(budgetSkip - 1);
        g_mode74DidDualThisFrame.store(true);  // HOLD last true L/R
        g_origRoot[1](self, edx);
        if (budgetSkip <= 3 || (budgetSkip % 8) == 0)
          Log("Mode75: DUAL BUDGET skip remain=%u (lastDual=%ums; hold L/R)",
              budgetSkip - 1, g_mode74LastDualMs.load());
        return;
      }
      const DWORD lastEsMs = g_mode74LastEsMs.load();
      if (lastEsMs >= kMode75SoftSkipMs) {
        g_mode74DidDualThisFrame.store(true);
        g_origRoot[1](self, edx);
        const uint32_t soft = g_mode74SoftSkipStreak.fetch_add(1) + 1;
        if (soft <= 4 || (soft % 60) == 0)
          Log("Mode75: SOFT SKIP dual (lastEs=%ums≥%u; hold L/R; streak=%u)", lastEsMs,
              kMode75SoftSkipMs, soft);
        if (soft >= kMode75SoftSkipToPause) {
          Mode74BeginHardOff("soft-skip streak (streaming stress)");
          g_mode74SoftSkipStreak.store(0);
        }
        return;
      }
      g_mode74SoftSkipStreak.store(0);
      g_mode74DidDualThisFrame.store(true);
      g_inDual.store(true);
      const DWORD dualT0 = GetTickCount();
      const bool ok = RunMode73SameFrameDualGuarded(self, edx);
      const DWORD dualMs = GetTickCount() - dualT0;
      g_mode74LastDualMs.store(dualMs);
      g_inDual.store(false);
      if (!ok) {
        g_mode74DualDead.store(true);
        g_mode74GateOpen.store(false);
        g_mode74DidDualThisFrame.store(false);
        g_haveL = g_haveR = false;
        Log("Mode75: EXCEPTION in SAME-FRAME dual — permanently disabled; wrote stereo=45");
        WriteStereoModeFile(45);
        g_origRoot[1](self, edx);
        return;
      }
      if (dualMs >= kMode75DualBudgetMs) {
        g_mode74DualSkipRemain.store(kMode75DualBudgetSkip);
        Log("Mode75: DUAL BUDGET arm skip=%u (dualMs=%ums≥%u; keep HOLD+Submit)",
            kMode75DualBudgetSkip, dualMs, kMode75DualBudgetMs);
      }
      const uint32_t n = ++g_mode74DualN;
      if (n <= 6 || (n % 60) == 0)
        Log("Mode75: SAFER SPARSE dual #%u haveL=%d haveR=%d injects=%u sep=%.0fcm "
            "pubProj=%u c0=%u dualMs=%u (1/%u BuildRootA×2; denser fearvr SCRAPPED; kill=45)",
            n, g_haveL ? 1 : 0, g_haveR ? 1 : 0, g_mode72Injects.load(),
            GetStereoSepMeters() * 100.f, g_mode74PublishProjInjects.load(),
            g_mode74PubC0Writes.load(), dualMs, kMode75DualEveryN);
      if (g_haveL && g_haveR && (n == 5 || (n % 300) == 0))
        CompareEyeCanvases(g_device);
      return;
    }
    // Off-tick: mono build; EndScene HOLDs last true pair while gate OPEN.
    g_mode74DidDualThisFrame.store(true);
  } else if (IsMode74Family(GetStereoMode())) {
    g_mode74DidDualThisFrame.store(false);
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
constexpr uint32_t kVsWrapRva = 0x2D2AC;  // mapped; old file-off doc 0x2C6AC FORBIDDEN
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

// Mode 69: SEH-safe read of VS-upload matrix at view+0x80. Never writes.
bool Mode69TryCopyMat(void* view, float out[16]) {
  __try {
    if (!view)
      return false;
    const auto* src =
        reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(view) + 0x80);
    for (int i = 0; i < 16; ++i)
      out[i] = src[i];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Mode69PeekViewMat(void* self) {
  float m[16]{};
  if (!Mode69TryCopyMat(self, m)) {
    ++g_mode69PeekFail;
    return;
  }
  ++g_mode69PeekOk;

  // Identity / near-identity → still menu/load. Live = translation or off-axis basis.
  const float tx = m[12], ty = m[13], tz = m[14];
  const float t2 = tx * tx + ty * ty + tz * tz;
  const float basisOff =
      (m[0] - 1.f) * (m[0] - 1.f) + m[1] * m[1] + m[2] * m[2] + m[4] * m[4] +
      (m[5] - 1.f) * (m[5] - 1.f) + m[6] * m[6] + m[8] * m[8] + m[9] * m[9] +
      (m[10] - 1.f) * (m[10] - 1.f);
  const bool looksLive = (t2 > 1.f) || (basisOff > 0.01f);

  if (looksLive && !g_mode69Live.load()) {
    g_mode69Live.store(true);
    Log("Mode69: LIVE matrix view=%p t=(%.3f,%.3f,%.3f) r0=(%.3f,%.3f,%.3f) "
        "r1=(%.3f,%.3f,%.3f) — starting %u-ES sample window",
        self, tx, ty, tz, m[0], m[1], m[2], m[4], m[5], m[6], kMode69PeekEsNeed);
    g_mode69LogLeft.store(8);
    g_mode69PeekEs.store(0);
    g_mode69MaxDelta = 0.f;
    g_mode69HaveLast = false;
  }

  if (g_mode69HaveLast) {
    float d = 0.f;
    for (int i = 0; i < 16; ++i) {
      const float t = m[i] - g_mode69LastMat[i];
      d += t * t;
    }
    if (d > g_mode69MaxDelta)
      g_mode69MaxDelta = d;
  }
  for (int i = 0; i < 16; ++i)
    g_mode69LastMat[i] = m[i];
  g_mode69HaveLast = true;

  if (!g_mode69Live.load())
    return;

  uint32_t left = g_mode69LogLeft.load();
  while (left > 0) {
    if (g_mode69LogLeft.compare_exchange_weak(left, left - 1)) {
      Log("Mode69: PEEK view=%p t=(%.3f,%.3f,%.3f) r0=(%.3f,%.3f,%.3f) "
          "ok=%u fail=%u (read-only dual=OFF)",
          self, m[12], m[13], m[14], m[0], m[1], m[2], g_mode69PeekOk.load(),
          g_mode69PeekFail.load());
      break;
    }
  }
}

bool Mode70ReadActiveView(uint32_t* outActive) {
  __try {
    *outActive = *reinterpret_cast<const uint32_t*>(kMode65ActiveViewVa);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Mode70PeekViewMat(void* self) {
  float m[16]{};
  if (!Mode69TryCopyMat(self, m))
    return;

  const float tx = m[12], ty = m[13], tz = m[14];
  const float t2 = tx * tx + ty * ty + tz * tz;
  const float basisOff =
      (m[0] - 1.f) * (m[0] - 1.f) + m[1] * m[1] + m[2] * m[2] + m[4] * m[4] +
      (m[5] - 1.f) * (m[5] - 1.f) + m[6] * m[6] + m[8] * m[8] + m[9] * m[9] +
      (m[10] - 1.f) * (m[10] - 1.f);
  const bool looksLive = (t2 > 1.f) || (basisOff > 0.01f);
  if (!looksLive)
    return;

  ++g_mode70LiveTotal;
  uint32_t active = 0;
  if (!Mode70ReadActiveView(&active) || !active ||
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)) != active) {
    ++g_mode70ActiveMiss;
    return;
  }
  ++g_mode70ActiveHit;

  if (!g_mode70Live.load()) {
    g_mode70Live.store(true);
    Log("Mode70: ACTIVE+LIVE view=%p active=0x%X t=(%.3f,%.3f,%.3f) — sample window %u ES",
        self, active, tx, ty, tz, kMode70PeekEsNeed);
    g_mode70LogLeft.store(8);
    g_mode70PeekEs.store(0);
    g_mode70MaxDelta = 0.f;
    g_mode70HaveLast = false;
  }

  if (g_mode70HaveLast) {
    float d = 0.f;
    for (int i = 0; i < 16; ++i) {
      const float t = m[i] - g_mode70LastMat[i];
      d += t * t;
    }
    if (d > g_mode70MaxDelta)
      g_mode70MaxDelta = d;
  }
  for (int i = 0; i < 16; ++i)
    g_mode70LastMat[i] = m[i];
  g_mode70HaveLast = true;

  uint32_t left = g_mode70LogLeft.load();
  while (left > 0) {
    if (g_mode70LogLeft.compare_exchange_weak(left, left - 1)) {
      Log("Mode70: PEEK activeHit view=%p t=(%.3f,%.3f,%.3f) hit=%u miss=%u liveTot=%u",
          self, m[12], m[13], m[14], g_mode70ActiveHit.load(), g_mode70ActiveMiss.load(),
          g_mode70LiveTotal.load());
      break;
    }
  }
}

bool Mode71WriteMat(void* view, const float m[16]) {
  __try {
    if (!view)
      return false;
    auto* dst = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(view) + 0x80);
    for (int i = 0; i < 16; ++i)
      dst[i] = m[i];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool Mode71MatSaneForInject(const float m[16]) {
  for (int i = 0; i < 16; ++i) {
    if (!std::isfinite(m[i]))
      return false;
  }
  const float tx = m[12], ty = m[13], tz = m[14];
  const float t2 = tx * tx + ty * ty + tz * tz;
  if (t2 < kMode71MinT2)
    return false;
  // Right row must be ~unit length (reject m0=10000 garbage that teleported cam).
  const float rlen = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  if (rlen < 0.5f || rlen > 1.5f)
    return false;
  return true;
}

bool Mode71CallOrigOnly(void* self, void* edx) {
  __try {
    g_origDrawWalk(self, edx);
    return true;
  } __except (ExecViewFilter(GetExceptionInformation())) {
    return false;
  }
}

// Inject ±half IPD along matrix right (m0..m2), call ReplayDispatch, ALWAYS restore.
bool Mode71InjectCallGuarded(void* self, void* edx) {
  float m[16]{};
  if (!Mode69TryCopyMat(self, m))
    return Mode71CallOrigOnly(self, edx);

  uint32_t active = 0;
  const bool isActive = Mode70ReadActiveView(&active) && active &&
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)) == active;
  if (!isActive || !Mode71MatSaneForInject(m)) {
    if (isActive)
      ++g_mode71RejectMat;
    else
      ++g_mode71Skip;
    return Mode71CallOrigOnly(self, edx);
  }

  // One inject per EndScene (temporal eye already latched). Prevents 200×/frame thrash.
  uint32_t budget = g_mode71Budget.load();
  if (budget == 0) {
    ++g_mode71Skip;
    return Mode71CallOrigOnly(self, edx);
  }
  if (!g_mode71Budget.compare_exchange_strong(budget, 0)) {
    ++g_mode71Skip;
    return Mode71CallOrigOnly(self, edx);
  }

  float backup[16];
  for (int i = 0; i < 16; ++i)
    backup[i] = m[i];

  float half = 0.5f * GetStereoSepMeters() * GetStereoScale();
  if (half > kMode71MaxHalfSep)
    half = kMode71MaxHalfSep;
  if (half < 0.f)
    half = 0.f;
  const float sign = (GetStereoEye() == StereoEye::Right) ? 1.f : -1.f;
  const float rlen = std::sqrt(backup[0] * backup[0] + backup[1] * backup[1] + backup[2] * backup[2]);
  const float inv = (rlen > 1e-3f) ? (1.f / rlen) : 0.f;
  const float rx = backup[0] * inv, ry = backup[1] * inv, rz = backup[2] * inv;
  m[12] = backup[12] + sign * half * rx;
  m[13] = backup[13] + sign * half * ry;
  m[14] = backup[14] + sign * half * rz;

  if (!Mode71WriteMat(self, m))
    return Mode71CallOrigOnly(self, edx);

  const bool ok = Mode71CallOrigOnly(self, edx);
  Mode71WriteMat(self, backup);  // ALWAYS restore — freeze root if left dirty after fault
  if (ok)
    ++g_mode71Injects;
  return ok;
}

// Mode 72: SEH-safe read of src floats (may be game memory). Never writes src.
bool Mode72TryCopySrc(const float* src, float out[16]) {
  __try {
    if (!src)
      return false;
    for (int i = 0; i < 16; ++i)
      out[i] = src[i];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool Mode72ReadActiveView(uint32_t* outActive) {
  __try {
    *outActive = *reinterpret_cast<const uint32_t*>(kMode65ActiveViewVa);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Live activeView, else cached (Mode74 dual when global is cleared mid-build).
uint32_t Mode74ResolveViewForInject(bool* usedCache) {
  if (usedCache)
    *usedCache = false;
  uint32_t live = 0;
  if (Mode72ReadActiveView(&live) && live) {
    g_mode74CachedView.store(live);
    return live;
  }
  const uint32_t cached = g_mode74CachedView.load();
  if (cached) {
    if (usedCache)
      *usedCache = true;
    ++g_mode74CachedViewHits;
    return cached;
  }
  return 0;
}

void Mode74BeginEyePubHold() {
  // Drop any leaked holds without restore (new eye / new dual) — walk end restores.
  g_mode74HeldPubN = 0;
  for (int i = 0; i < kMode74MaxHeldPub; ++i) {
    g_mode74HeldPub[i].self = nullptr;
    g_mode74HeldPub[i].held = false;
  }
  // Fresh content-inject budget for this eye BuildRootA.
  g_mode74DualViewBudget.store(kMode74DualViewBudgetPerEye);
}

void Mode74RestoreHeldPub180() {
  for (int i = 0; i < g_mode74HeldPubN; ++i) {
    auto& h = g_mode74HeldPub[i];
    if (!h.held || !h.self)
      continue;
    __try {
      auto* p180 = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(h.self) + 0x180);
      for (int j = 0; j < 16; ++j)
        p180[j] = h.p180[j];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    h.held = false;
    h.self = nullptr;
  }
  g_mode74HeldPubN = 0;
}

bool Mode74HoldPub180(void* self, const float* backup180) {
  if (!self || !backup180)
    return false;
  for (int i = 0; i < g_mode74HeldPubN; ++i) {
    if (g_mode74HeldPub[i].self == self)
      return true;  // already holding original mono for this view
  }
  if (g_mode74HeldPubN >= kMode74MaxHeldPub)
    return false;
  auto& h = g_mode74HeldPub[g_mode74HeldPubN++];
  h.self = self;
  for (int i = 0; i < 16; ++i)
    h.p180[i] = backup180[i];
  h.held = true;
  return true;
}

void Mode72CallOrig(void* device, uint32_t startReg, const float* src, uint32_t count) {
  __try {
    g_origMode72SetVS(device, startReg, src, count);
  } __except (ExecViewFilter(GetExceptionInformation())) {
    g_mode72Dead.store(true);
    Log("Mode72: EXCEPTION in SetVSConstF code=0x%08X — will kill to 45", g_execViewExcCode);
  }
}

// Mode74: stamp HMD orientation onto LOCAL Rage cam matrix (view+0x80 layout:
// row0=right, row1=forward, row2=up, row3=pos). Never writes live view+0x80.
//
// ROOT (user 2026-07-25): dualEn=0 skipped this and only ±IPD/toe-in'd the game's
// body/follow basis → drawn image stayed head-locked ("world moves with head" /
// giant cinema). CamMatrix CCam bake often does NOT reach ReplayDispatch upload.
//
// Orient + cheap seated 6DoF translation on DRAW-PATH (CamMatrix often never reaches
// ReplayDispatch). ±IPD+toe-in via OffsetViewLocal. Once per inject; pose is frozen/ES.
// Returns true when basis was replaced (caller still runs OffsetViewLocal).
bool Mode74ApplyHmdEyeLocal(float* m) {
  vr::HmdMatrix34_t h{};
  if (!GetFrameHmdPoseMatrix(&h) || !m)
    return false;
  // OpenVR → GTA: (x,y,z)_gta = (x, -z, y)_ovr
  auto ovrToGta = [](float ox, float oy, float oz, float* gx, float* gy, float* gz) {
    *gx = ox;
    *gy = -oz;
    *gz = oy;
  };
  auto norm = [](float* x, float* y, float* z) {
    const float len = std::sqrt((*x) * (*x) + (*y) * (*y) + (*z) * (*z));
    if (len < 1e-6f) {
      *x = 0.f;
      *y = 1.f;
      *z = 0.f;
      return;
    }
    *x /= len;
    *y /= len;
    *z /= len;
  };

  // BEFORE basis (game body/follow upload) — proof of composition fix.
  const float beforeRx = m[0], beforeRy = m[1], beforeRz = m[2];
  const float beforeFx = m[4], beforeFy = m[5], beforeFz = m[6];

  // Head forward / right from FROZEN HMD pose (OpenVR -Z = forward).
  float hfx, hfy, hfz;
  ovrToGta(-h.m[0][2], -h.m[1][2], -h.m[2][2], &hfx, &hfy, &hfz);
  norm(&hfx, &hfy, &hfz);
  float hrx, hry, hrz;
  ovrToGta(h.m[0][0], h.m[1][0], h.m[2][0], &hrx, &hry, &hrz);
  float hrlen = std::sqrt(hrx * hrx + hry * hry + hrz * hrz);
  if (hrlen < 1e-4f) {
    hrx = hfy;
    hry = -hfx;
    hrz = 0.f;
    hrlen = std::sqrt(hrx * hrx + hry * hry + hrz * hrz);
  }
  if (hrlen > 1e-4f) {
    hrx /= hrlen;
    hry /= hrlen;
    hrz /= hrlen;
  } else {
    hrx = 1.f;
    hry = 0.f;
    hrz = 0.f;
  }
  // World-up rebuild (same as CamMatrix) — drop HMD roll for Rage cam stability.
  float rx = hfy;
  float ry = -hfx;
  float rz = 0.f;
  float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
  if (rlen < 1e-4f) {
    rx = hrx;
    ry = hry;
    rz = hrz;
  } else {
    rx /= rlen;
    ry /= rlen;
    rz /= rlen;
  }
  float ux = ry * hfz - rz * hfy;
  float uy = rz * hfx - rx * hfz;
  float uz = rx * hfy - ry * hfx;
  norm(&ux, &uy, &uz);

  // Keep ped-eye translation, then add FROZEN seated 6DoF (Mode74SyncFrameDesc).
  // DRAW-PATH owns this — CamMatrix bake usually does not reach this upload.
  m[0] = rx;
  m[1] = ry;
  m[2] = rz;
  m[4] = hfx;
  m[5] = hfy;
  m[6] = hfz;
  m[8] = ux;
  m[9] = uy;
  m[10] = uz;
  float sdx = 0.f, sdy = 0.f, sdz = 0.f;
  if (g_mode74FrameDesc.seatedValid) {
    sdx = g_mode74FrameDesc.seatedDx;
    sdy = g_mode74FrameDesc.seatedDy;
    sdz = g_mode74FrameDesc.seatedDz;
  }
  if (sdx != 0.f || sdy != 0.f || sdz != 0.f || g_mode74FrameDesc.seatedValid) {
    const float lean = GetLeanGain();
    const float g = (lean > 0.05f) ? (1.f / lean) : 1.f;
    m[12] += sdx * g;
    m[13] += sdy * g;
    m[14] += sdz * g;
  }

  static uint32_t s_hmdEyeN = 0;
  const uint32_t n = ++s_hmdEyeN;
  if (n == 1)
    Log("HmdLook: ACTIVE Mode74 DRAW-PATH (src-copy HMD orient + seated 6DoF; "
        "IPD+toeIn separate; never view+0x80; F9 recenter)");
  if (n <= 5 || (n % 1200) == 0) {
    const float yawBefore = std::atan2(beforeFx, beforeFy) * (180.f / 3.14159265f);
    const float yawAfter = std::atan2(hfx, hfy) * (180.f / 3.14159265f);
    const float pitchAfter =
        std::asin((std::max)(-1.f, (std::min)(1.f, hfz))) * (180.f / 3.14159265f);
    Log("HmdLook: DRAW stamp #%u beforeR=(%.2f,%.2f,%.2f) beforeF=(%.2f,%.2f,%.2f) "
        "afterR=(%.2f,%.2f,%.2f) afterF=(%.2f,%.2f,%.2f) yaw %.1f->%.1f pitch=%.1f "
        "seat=(%.2f,%.2f,%.2f) (world-stable look + lean; body basis replaced)",
        n, beforeRx, beforeRy, beforeRz, beforeFx, beforeFy, beforeFz, rx, ry, rz, hfx, hfy,
        hfz, yawBefore, yawAfter, pitchAfter, sdx, sdy, sdz);
  }
  return true;
}

const float* Mode72SelectSrc(const float* src, uint32_t startReg, uint32_t count,
                             void* gameRetAddr) {
  const StereoMode sm = GetStereoMode();
  const bool mode72 = (sm == StereoMode::VsConstTemporalInject);
  const bool mode73 = (sm == StereoMode::SameFrameVsConstDual);
  const bool mode74 = IsMode74Family(sm);
  if (!mode72 && !mode73 && !mode74) {
    ++g_mode72Skip;
    return src;
  }

  // UploadFn already stereo'd live +0x80 for this call — do not double-inject.
  if (mode74 && g_mode74UploadFnOwnsEye.load()) {
    ++g_mode72Skip;
    return src;
  }

  // Mode74 dual: also catch projection / alternate-reg uploads (not only view+0x80 @c0).
  // PublishSync builds P at +0x180 and VP at +0xC0; some paths may upload those.
  const bool dualOpen = mode73 || (mode74 && g_inDual.load());
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uint32_t retRva =
      (base && gameRetAddr)
          ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(gameRetAddr) - base)
          : 0;
  // World-draw upload returns inside 0x2A1E10 (Mode42 OWNER-EDGE). Prefer these.
  const bool uploadFnRet = (retRva == 0x2A2183u || retRva == 0x2A25FFu);
  const bool replayRet = (retRva == 0x30D13u);
  // HMD GetProjectionRaw → drawn proj upload.
  // Temporal (dual OFF): PubProj HOLD owns FOV — skip EyeProj discover/inject entirely
  // (was ~17M "see" checks + 45k log lines → street hitch via log I/O).
  // Dual OPEN / Mode78: keep EyeProj for L/R. Aggressive FOV: only until proven
  // (Mode80 hitch) — then dual-only. Mode83/84 quiet: same path via WantsQuietFovProof.
  // Mode86: NEVER mid-draw EyeProj (even in dual) — HMD proj vs engine cull = sidewalk holes.
  const bool needFovProof =
      (WantsAggressiveFovPresence(sm) || WantsQuietFovProof(sm)) && !IsDrawnFovProven();
  const bool wantEyeProj =
      !WantsNoMidDrawEyeProj(sm) &&
      (dualOpen || g_inDual.load() || IsShaderConstStereo(sm) || needFovProof);
  if (mode74 && wantEyeProj && src && count == 16 && g_mode74ProjCacheOk.load()) {
    bool usedCache = false;
    uint32_t active = Mode74ResolveViewForInject(&usedCache);
    const bool haveActive = active != 0;
    const uintptr_t srcU = reinterpret_cast<uintptr_t>(src);
    int off = -1;
    if (haveActive) {
      const intptr_t d =
          static_cast<intptr_t>(srcU) - static_cast<intptr_t>(active);
      if (d >= 0 && d <= 0x400)
        off = static_cast<int>(d);
    }
    const bool looksProj = Mode74LooksLikeProjection(src);
    // Require real proj signature OR known PublishSync proj slot — do not fire on bare reg.
    const bool slotProj = looksProj || (off == 0x180);
    const uint32_t disc = g_mode74ProjDiscoverN.fetch_add(1) + 1;
    // Quieter discover spam (Mode88 hitch cut): every 25k after warmup.
    const uint32_t discEvery =
        (IsHitchCutEyeProj(sm) || IsCullSyncPresence(sm) || IsConfigEyeRtLetterbox(sm))
            ? 25000u
            : 5000u;
    if (disc <= 8 || (disc % discEvery) == 0) {
      const char* offTag = "?";
      if (off == 0x80)
        offTag = "+0x80";
      else if (off == 0xC0)
        offTag = "+0xC0";
      else if (off == 0x100)
        offTag = "+0x100";
      else if (off == 0x180)
        offTag = "+0x180";
      else if (off == 0x1C0)
        offTag = "+0x1C0";
      else if (off == 0x308)
        offTag = "+0x308";
      else if (off >= 0)
        offTag = "other";
      Log("EyeProj: see reg=%u off=%s looksProj=%d dual=%d n=%u", startReg, offTag,
          looksProj ? 1 : 0, dualOpen ? 1 : 0, disc);
    }
    if (slotProj && Mode74GetCachedEyeProj(g_mode74LocalProj)) {
      // Prefer adapting the live upload as template (game near/far) when it looks like proj.
      if (looksProj)
        BuildHmdEyeProjection(
            (GetStereoEye() == StereoEye::Right) ? vr::Eye_Right : vr::Eye_Left,
            g_mode74LocalProj, src);
      ++g_mode74ProjInjects;
      const uint32_t n = g_mode74ProjInjects.load();
      if (!IsDrawnFovProven() && n >= 16) {
        SetDrawnFovProven(true);
        PublishGameFovFromDrawnSy(g_mode74LocalProj[0], g_mode74LocalProj[5],
                                  GetBackbufferAspect());
        Log("FOVPROOF: DRAWN via EyeProj inject n=%u sx=%.3f sy=%.3f — canvas ungated", n,
            g_mode74LocalProj[0], g_mode74LocalProj[5]);
      }
      const uint32_t injEvery =
          (IsHitchCutEyeProj(sm) || IsCullSyncPresence(sm)) ? 2400u : 600u;
      if (n <= 4 || (n % injEvery) == 0)
        Log("EyeProj: INJECT src-copy #%u eye=%s reg=%u off=%d sx=%.3f ox=%.3f dual=%d "
            "(HMD FOV on draw upload; never view+0x80)",
            n, (GetStereoEye() == StereoEye::Right) ? "R" : "L", startReg, off,
            g_mode74LocalProj[0], g_mode74LocalProj[8], dualOpen ? 1 : 0);
      return g_mode74LocalProj;
    }
    if (slotProj)
      ++g_mode74ProjMiss;
  }

  if (count != 16 || startReg != 0 || !src) {
    ++g_mode72Skip;
    return src;
  }
  bool usedCache = false;
  uint32_t active = 0;
  // Mode74: always resolve live+cache (activeView often 0 mid-draw). Do not early-out —
  // contentHit covers 0x2A1E10 MatMul stack uploads that are not activeView+0x80.
  if (mode74)
    active = Mode74ResolveViewForInject(&usedCache);
  else if (!Mode72ReadActiveView(&active) || !active) {
    ++g_mode72Skip;
    return src;
  }

  float m[16]{};
  const bool copied = Mode72TryCopySrc(src, m);
  const bool sane = copied && Mode71MatSaneForInject(m);
  bool pointerHit = false;
  if (active) {
    const auto* expect =
        reinterpret_cast<const float*>(static_cast<uintptr_t>(active) + 0x80);
    pointerHit = (src == expect);
  }

  // Mode74 presence (dual OR crash-safe temporal): if activeView+0x80 pointer miss,
  // inject any sane near-cam view upload (0x2A1E10 stack / alternate bases). Cap via
  // DualViewBudget so we never inject-every (~300/frame hang). Never write view+0x80.
  // Prefer uploadFnRet when present; ReplayDispatch 0x30D13 is the CE hot path (UploadFn
  // often silent) — keep injecting there unless OwnsEye already stereo'd this upload.
  bool contentHit = false;
  if (mode74 && !pointerHit && sane) {
    if (Mode74ViewNearLiveCam(m)) {
      uint32_t budget = g_mode74DualViewBudget.load();
      if (budget > 0 && g_mode74DualViewBudget.compare_exchange_strong(budget, budget - 1)) {
        contentHit = true;
        const uint32_t baseView =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(src) - 0x80);
        if (baseView)
          g_mode74CachedView.store(baseView);
      }
    }
  }

  if (!pointerHit && !contentHit) {
    ++g_mode72Skip;
    return src;
  }
  if (!sane) {
    ++g_mode72RejectMat;
    return src;
  }
  // Mode 72 classic: 1 inject/ES.
  // Mode74: DualViewBudget gates content; pointer hits also take that budget so the
  // first non-world +0x80 upload cannot starve the real 0x2A1E10 draw upload.
  // Mode 73 / Mode74 g_inDual: inject every match (budget refreshed per eye walk).
  if (!dualOpen) {
    if (mode74) {
      if (!contentHit) {
        uint32_t budget = g_mode74DualViewBudget.load();
        if (budget == 0 ||
            !g_mode74DualViewBudget.compare_exchange_strong(budget, budget - 1)) {
          ++g_mode72Skip;
          return src;
        }
      }
    } else {
      uint32_t budget = g_mode72Budget.load();
      if (budget == 0 || !g_mode72Budget.compare_exchange_strong(budget, 0)) {
        ++g_mode72Skip;
        return src;
      }
    }
  }
  for (int i = 0; i < 16; ++i)
    g_mode72LocalMat[i] = m[i];
  // Mode74 DRAW-PATH look (dualEn=0 OR dual): ALWAYS stamp HMD orient on src-copy.
  // Prior dualEn=0 path assumed CamMatrix CCam bake reached ReplayDispatch — it does
  // not (contentInj=0; body/follow basis survived) → head-locked giant screen.
  // Then ±IPD + soft toe-in. Never write live view+0x80. No BuildRootA×2 here.
  if (mode74)
    Mode74ApplyHmdEyeLocal(g_mode72LocalMat);
  Mode74OffsetViewLocal(g_mode72LocalMat, GetStereoEye() == StereoEye::Right);
  ++g_mode72Injects;
  if (usedCache && pointerHit)
    ++g_mode74CachedViewInjects;
  if (contentHit)
    ++g_mode74ContentInjects;
  const uint32_t nInj = g_mode72Injects.load();
  if (mode74 && (nInj <= 4 || (nInj % 600) == 0)) {
    const float dx = g_mode72LocalMat[12] - m[12];
    const float dy = g_mode72LocalMat[13] - m[13];
    const float dz = g_mode72LocalMat[14] - m[14];
    const float sep = std::sqrt(dx * dx + dy * dy + dz * dz);
    Log("Mode74: VIEW inject #%u eye=%s ptr=%d cache=%d content=%d view=0x%X "
        "eyeSep=%.1fcm dual=%d ret=0x%X uploadFnRet=%d "
        "(src-copy near-cam; never view+0x80)",
        nInj, (GetStereoEye() == StereoEye::Right) ? "R" : "L", pointerHit ? 1 : 0,
        (usedCache && pointerHit) ? 1 : 0, contentHit ? 1 : 0, active, sep * 100.f,
        dualOpen ? 1 : 0, retRva, uploadFnRet ? 1 : 0);
  }
  return g_mode72LocalMat;
}

// After a Mode74 view inject: if device VS regs still hold a game projection, replace
// with cached HMD eye proj (src-copy style on the constant bank — never view+0x80).
void Mode74PatchDeviceProjIfPresent(void* device) {
  if (!device || !g_mode74ProjCacheOk.load())
    return;
  const StereoMode sm = GetStereoMode();
  // Mode86: never rewrite mid-draw VS proj (frustum-cull hole source).
  if (WantsNoMidDrawEyeProj(sm))
    return;
  // Dual Mode75 always; Mode78; aggressive/quiet FOV only until proven OR in dual.
  const bool needFovProofPatch =
      (WantsAggressiveFovPresence(sm) || WantsQuietFovProof(sm)) && !IsDrawnFovProven();
  if (!g_inDual.load() && !IsShaderConstStereo(sm) && !needFovProofPatch)
    return;
  auto* dev = reinterpret_cast<IDirect3DDevice9*>(device);
  float hmd[16]{};
  if (!Mode74GetCachedEyeProj(hmd))
    return;
  int patched = 0;
  for (UINT base = 0; base <= 32; base += 4) {
    float block[16]{};
    if (FAILED(dev->GetVertexShaderConstantF(base, block, 4)))
      continue;
    if (!Mode74LooksLikeProjection(block))
      continue;
    float adapted[16]{};
    if (!BuildHmdEyeProjection(
            (GetStereoEye() == StereoEye::Right) ? vr::Eye_Right : vr::Eye_Left, adapted,
            block))
      continue;
    if (FAILED(dev->SetVertexShaderConstantF(base, adapted, 4)))
      continue;
    ++patched;
  }
  if (patched > 0) {
    const uint32_t n = g_mode74ProjDevicePatches.fetch_add(static_cast<uint32_t>(patched)) +
                       static_cast<uint32_t>(patched);
    // Mid-draw VS proj rewrite IS the drawn FOV owner — ungated canvas once stable.
    if (!IsDrawnFovProven() && n >= 24) {
      SetDrawnFovProven(true);
      // Prefer measured HMD/adapted scales — never claim CCam wider than drawn.
      PublishGameFovFromDrawnSy(hmd[0], hmd[5], GetBackbufferAspect());
      Log("FOVPROOF: DRAWN via device VS patch n=%u sx=%.3f sy=%.3f — canvas ungated "
          "(EndScene scan often empty; mid-draw owns proj)",
          n, hmd[0], hmd[5]);
    }
    if (n <= 4 || (n % 300) == 0)
      Log("EyeProj: device VS patch +%d (total~%u) eye=%s (Get/Set const; never view+0x80)",
          patched, n, (GetStereoEye() == StereoEye::Right) ? "R" : "L");
  }
}

void __stdcall HookMode72SetVSConstF(void* device, uint32_t startReg, const float* src,
                                     uint32_t count) {
  if (!g_origMode72SetVS)
    return;
  const StereoMode sm = GetStereoMode();
  if ((sm != StereoMode::VsConstTemporalInject && sm != StereoMode::SameFrameVsConstDual &&
       !IsMode74Family(sm)) ||
      !g_mode72Armed.load() || g_mode72Dead.load()) {
    g_origMode72SetVS(device, startReg, src, count);
    return;
  }
  void* gameRet = _ReturnAddress();
  const float* useSrc = Mode72SelectSrc(src, startReg, count, gameRet);
  Mode72CallOrig(device, startReg, useSrc, count);
  // After view upload: if device VS regs still hold a game projection, replace it.
  // Dual always; Mode78; aggressive/quiet FOV only until proven (then dual-only).
  // Mode86: never (WantsNoMidDrawEyeProj).
  const bool needPatch =
      !WantsNoMidDrawEyeProj(sm) &&
      (g_inDual.load() || IsShaderConstStereo(sm) ||
       ((WantsAggressiveFovPresence(sm) || WantsQuietFovProof(sm)) && !IsDrawnFovProven()));
  if (useSrc == g_mode72LocalMat && needPatch) {
    Mode74PatchDeviceProjIfPresent(device);
  }
}

bool Mode72InstallHook();
void Mode72OnEndScene(IDirect3DDevice9* device);
void Mode74OnEndScene(IDirect3DDevice9* device);

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

  // Mode 64: ViewConst COUNT-only (no dual).
  if (GetStereoMode() == StereoMode::ViewConstCountProbe && !g_mode64CountDone.load()) {
    if (!CallDrawWalkOnceGuarded(self, edx)) {
      Log("Mode64: EXCEPTION in count passthrough code=0x%08X @exeRva=0x%X",
          g_execViewExcCode, g_mode64SiteRva.load());
      g_drawWalkEntries.store(0xFFFFFFFFu);
    }
    return;
  }

  // Mode 66: PublishSync COUNT-only (no dual).
  if (GetStereoMode() == StereoMode::PublishSyncCountProbe && !g_mode66CountDone.load()) {
    if (!CallDrawWalkOnceGuarded(self, edx)) {
      Log("Mode66: EXCEPTION in count passthrough code=0x%08X @exeRva=0x%X",
          g_execViewExcCode, g_mode66SiteRva.load());
      g_drawWalkEntries.store(0xFFFFFFFFu);
    }
    return;
  }

  // Mode 67: ViewMatWriter COUNT-only (no dual).
  if (GetStereoMode() == StereoMode::ViewMatWriterCountProbe && !g_mode67CountDone.load()) {
    if (!CallDrawWalkOnceGuarded(self, edx)) {
      Log("Mode67: EXCEPTION in count passthrough code=0x%08X @exeRva=0x%X",
          g_execViewExcCode, g_mode67SiteRva.load());
      g_drawWalkEntries.store(0xFFFFFFFFu);
    }
    return;
  }

  // Mode 68: ReplayDispatch COUNT-only (no dual).
  if (GetStereoMode() == StereoMode::ReplayDispatchCountProbe && !g_mode68CountDone.load()) {
    if (!CallDrawWalkOnceGuarded(self, edx)) {
      Log("Mode68: EXCEPTION in count passthrough code=0x%08X @exeRva=0x%X",
          g_execViewExcCode, g_mode68SiteRva.load());
      g_drawWalkEntries.store(0xFFFFFFFFu);
    }
    return;
  }

  // Mode 69: ReplayDispatch matrix peek (read-only; no dual).
  if (GetStereoMode() == StereoMode::ReplayDispatchMatPeek && !g_mode69PeekDone.load()) {
    Mode69PeekViewMat(self);
    if (!CallDrawWalkOnceGuarded(self, edx)) {
      Log("Mode69: EXCEPTION in peek passthrough code=0x%08X @exeRva=0x%X",
          g_execViewExcCode, g_mode69SiteRva.load());
      g_drawWalkEntries.store(0xFFFFFFFFu);
    }
    return;
  }

  // Mode 70: activeView-filtered matrix peek (read-only; no dual).
  if (GetStereoMode() == StereoMode::ReplayDispatchActivePeek && !g_mode70PeekDone.load()) {
    Mode70PeekViewMat(self);
    if (!CallDrawWalkOnceGuarded(self, edx)) {
      Log("Mode70: EXCEPTION in peek passthrough code=0x%08X @exeRva=0x%X",
          g_execViewExcCode, g_mode70SiteRva.load());
      g_drawWalkEntries.store(0xFFFFFFFFu);
    }
    return;
  }

  // Mode 71: temporal L/R inject (write+restore; no same-frame dual).
  if (GetStereoMode() == StereoMode::ReplayDispatchTemporalInject && g_mode71Armed.load() &&
      !g_mode71Dead.load()) {
    if (!Mode71InjectCallGuarded(self, edx)) {
      Log("Mode71: EXCEPTION inject code=0x%08X @exeRva=0x%X — kill stereo=45",
          g_execViewExcCode, g_mode71SiteRva.load());
      g_drawWalkEntries.store(0xFFFFFFFFu);
      g_mode71Dead.store(true);
    }
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
constexpr uint32_t kForbiddenHookRvaA = 0x2D2AC;  // VS wrap — Mode 28 crash (mapped)
constexpr uint32_t kForbiddenHookRvaB = 0x37BD0;  // wrong-ABI — Mode 27/29 crash
constexpr uint32_t kForbiddenMidRva = 0x37C01;    // resolves to 0x37BD0
constexpr uint32_t kMode31DiscoverEs = 90;
constexpr uint32_t kMode31CountEsNeed = 45;

bool Mode31ForbiddenRva(uint32_t rva) {
  return rva == kForbiddenHookRvaA || rva == kForbiddenHookRvaB || rva == kForbiddenMidRva ||
         rva == 0x2D33E || rva == 0x2C73E || rva == 0x2C6AC;
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
         rva == 0x2D33E || rva == 0x2C73E || rva == 0x2C6AC;
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
void TemporalCaptureAerMode74(IDirect3DDevice9* device);

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
constexpr uint32_t kMode34VsRetRva = 0x2D33E;  // mapped; old file-off doc 0x2C73E

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
  // Mapped (+0xC00) and legacy file-off forms both rejected.
  return Mode33ForbiddenRva(rva) || rva == kMode34VsRetRva || rva == 0x2C73E ||
         rva == 0x1BF010 || rva == 0x1BFC10 || rva == 0x52E7C0 || rva == 0x52F3C0 ||
         rva == 0x4DDAD0 || rva == 0x4DE6D0;
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
  return rva == 0x2D2AC || rva == 0x387BD0 || rva == 0x1BFC10 || rva == 0x4DE6D0 ||
         rva == 0x2C6AC || rva == 0x37BD0 || rva == 0x1BF010 || rva == 0x4DDAD0;
  // Include both mapped (+0xC00) and legacy file-off RVAs so old probes still reject.
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
  const char* label = mode42 ? "Mode42" : "Mode41";
  Log("%s: replay-chain epoch=%u VsRetHits=%u nodes=%d tid=%u SameFrame=0 distinctEyes=0",
      label, es, g_mode41VsRetHits.load(), g_mode41AggN, g_mode41VsTid);
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
    // ReplayGate-guarded +178 triad (stereo-relevant of 12 exe-wide sites):
    //   0x30D0D → ret 0x30D13 (ReplayDispatch)
    //   0x2A217D → ret 0x2A2183 (upload fn 0x2A1E10 path A)
    //   0x2A25F9 → ret 0x2A25FF (upload fn 0x2A1E10 path B)
    // READ-ONLY log only — SameFrameSeamGate stays CLOSED.
    if (node.retRva == 0x30D13 || node.retRva == 0x2A2183 || node.retRva == 0x2A25FF) {
      uint32_t owner = fnRva;
      uint32_t expectCall = 0;
      const char* path = "?";
      if (node.retRva == 0x30D13) {
        expectCall = 0x30D0D;
        owner = (callRva == expectCall) ? 0x30CD0u : fnRva;
        path = "ReplayDispatch";
      } else if (node.retRva == 0x2A2183) {
        expectCall = 0x2A217D;
        owner = (callRva == expectCall) ? 0x2A1E10u : fnRva;
        path = "UploadA";
      } else {
        expectCall = 0x2A25F9;
        owner = (callRva == expectCall) ? 0x2A1E10u : fnRva;
        path = "UploadB";
      }
      Log("%s: OWNER-EDGE ret=0x%X enclosing=0x%X abi=%s call=%s@0x%X "
          "modrm=0x%02X len=%u cadenceFrames=%u hits=%u path=%s expectCall=0x%X "
          "match=%d hook=NO replay=NO SameFrameSeamGate=CLOSED",
          label, node.retRva, owner, abi, call, callRva, modrm, callLen, node.frames,
          node.hits, path, expectCall, (callRva == expectCall) ? 1 : 0);
    }
    // Mapped PublishSync return sites (ViewConst @0x327FF / ViewMat @0x31624 → 0x30F00).
    if (node.retRva == 0x32804 || node.retRva == 0x31629) {
      Log("%s: PUBLISH-RET ret=0x%X call=%s@0x%X->0x%X (ViewConst/ViewMat -> PublishSync; "
          "mapped; SameFrameSeamGate=CLOSED) hook=NO",
          label, node.retRva, call, callRva, targetRva);
    }
    // Old file-off "0x3199A/A4" mid-rets are SSE mid-instr at mapped 0x3259A/A4 — not seams.
    if (node.retRva == 0x3259A || node.retRva == 0x325A4 || node.retRva == 0x3199A ||
        node.retRva == 0x319A4) {
      Log("%s: NOTE ret=0x%X is ViewConst MID-SSE (not a call return; ignore as seam)", label,
          node.retRva);
    }
  }
}

void Mode41OnEndScene(IDirect3DDevice9* device) {
  TemporalCapturePairHold(device);  // Same safe behavior as Mode 40.
  Mode41FinishFrame();
}

bool Mode64InstallCountHook() {
  const ReSiteSpec spec{ "ViewConst", kMode64Pattern, kMode64ViewConstExpectedRva, kMode64Prologue,
                         sizeof(kMode64Prologue), /*requireCcPad=*/true };
  const uint32_t siteRva = ResolveReSite(spec);
  g_mode64SiteRva.store(siteRva);
  if (!siteRva) {
    Log("Mode64: REJECT — ResolveReSite failed (expected mapped TRUE start 0x%X; mid 0x3247C "
        "is unsafe)",
        kMode64ViewConstExpectedRva);
    return false;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t start = base + siteRva;
  const char* tag = "?";
  if (Mode32ClassifyPrologue(start, &tag) != Mode32Abi::Thiscall) {
    Log("Mode64: REJECT exeRva=0x%X abi=%s (need thiscall ViewConst)", siteRva, tag);
    return false;
  }
  Mode34WriteProbe(siteRva);
  if (!InstallDrawWalkAt(start)) {
    Mode34ClearProbe("MH fail");
    Log("Mode64: InstallDrawWalkAt failed exeRva=0x%X", siteRva);
    return false;
  }
  g_mode64Es.store(0);
  g_mode64CountEs.store(0);
  g_mode64EntrySum = 0;
  g_mode64CountDone.store(false);
  g_drawWalkEntries.store(0);
  Log("Mode64: COUNT armed exeRva=0x%X abi=%s (ViewConst mapped TRUE start; sole E8 @0x9777C2 "
      "gated by [0x1797694]; want avg in [0.8,4] over %u EndScenes; dual=OFF "
      "SameFrameSeamGate=CLOSED)",
      siteRva, tag, kMode64CountEsNeed);
  return true;
}

void Mode64OnEndScene(IDirect3DDevice9* device) {
  TemporalCapturePairHold(device);
  if (g_mode64CountDone.load())
    return;

  const uint32_t siteRva = g_mode64SiteRva.load();
  const uint32_t entries = g_drawWalkEntries.exchange(0);
  if (entries == 0xFFFFFFFFu) {
    Log("Mode64: COUNT hook died @exeRva=0x%X — wrote stereo=45 (safe default)", siteRva);
    Mode34ClearProbe("count died");
    if (g_drawWalkAddr) {
      MH_DisableHook(g_drawWalkAddr);
      MH_RemoveHook(g_drawWalkAddr);
      g_origDrawWalk = nullptr;
      g_drawWalkAddr = nullptr;
    }
    g_mode64CountDone.store(true);
    WriteStereoModeFile(45);
    return;
  }

  ++g_mode64CountEs;
  g_mode64EntrySum += entries;
  const uint32_t n = g_mode64CountEs.load();
  if (n <= 6 || (n % 15) == 0)
    Log("Mode64: COUNT es#%u/%u entries=%u sum=%u exeRva=0x%X", n, kMode64CountEsNeed, entries,
        g_mode64EntrySum, siteRva);

  if (n < kMode64CountEsNeed)
    return;

  const float avg = static_cast<float>(g_mode64EntrySum) / static_cast<float>(n);
  Mode34ClearProbe("COUNT done");
  if (g_drawWalkAddr) {
    MH_DisableHook(g_drawWalkAddr);
    MH_RemoveHook(g_drawWalkAddr);
    g_origDrawWalk = nullptr;
    g_drawWalkAddr = nullptr;
  }
  g_mode64CountDone.store(true);
  const bool okCadence = avg >= 0.8f && avg <= 4.f;
  Log("Mode64: COUNT done exeRva=0x%X avgEntries=%.2f cadence=%s (want [0.8,4] ~1x/frame) "
      "SameFrameSeamGate=CLOSED dual=OFF",
      siteRva, avg, okCadence ? "PASS" : "REJECT");
  if (okCadence)
    Log("Mode64: next live step — stereo=41 then 42 (ladder: 66→67→65→64 done)");
  else
    Log("Mode64: cadence REJECT — not ~1x/frame (or ViewConst gate [0x1797694] stays 0; "
        "gate!=0 required; no static writer; prefer 66/67 already run); keep stereo=45");
  WriteStereoModeFile(45);
}

bool Mode66InstallCountHook() {
  // PublishSync sits after prior epilogue (not CC-pad) — requireCcPad=false.
  const ReSiteSpec spec{ "PublishSync", kMode66Pattern, kMode66PublishSyncExpectedRva,
                         kMode66Prologue, sizeof(kMode66Prologue), /*requireCcPad=*/false };
  const uint32_t siteRva = ResolveReSite(spec);
  g_mode66SiteRva.store(siteRva);
  if (!siteRva) {
    Log("Mode66: REJECT — ResolveReSite failed (expected mapped RVA 0x%X; old file-off "
        "0x30300 was wrong)",
        kMode66PublishSyncExpectedRva);
    return false;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t start = base + siteRva;
  if (Mode34ForbiddenRva(siteRva)) {
    Log("Mode66: REJECT exeRva=0x%X forbidden", siteRva);
    return false;
  }
  const char* tag = "?";
  if (Mode32ClassifyPrologue(start, &tag) != Mode32Abi::Thiscall) {
    Log("Mode66: REJECT exeRva=0x%X abi=%s (need thiscall PublishSync)", siteRva, tag);
    return false;
  }
  Mode34WriteProbe(siteRva);
  if (!InstallDrawWalkAt(start)) {
    Mode34ClearProbe("MH fail");
    Log("Mode66: InstallDrawWalkAt failed exeRva=0x%X", siteRva);
    return false;
  }
  g_mode66Es.store(0);
  g_mode66CountEs.store(0);
  g_mode66EntrySum = 0;
  g_mode66CountDone.store(false);
  g_drawWalkEntries.store(0);
  Log("Mode66: COUNT armed exeRva=0x%X abi=%s (PublishSync mapped; 12 E8 sites: "
      "11+PublishProj + 1 inside helper 0x31940; helper also x58 E8 — avg may be >4 SHARED; "
      "upload fn 0x2A1E10 has two +178 paths (0x2A217D/0x2A25F9); "
      "dual=OFF gate=CLOSED; %u EndScenes)",
      siteRva, tag, kMode66CountEsNeed);
  return true;
}

void Mode66OnEndScene(IDirect3DDevice9* device) {
  TemporalCapturePairHold(device);
  if (g_mode66CountDone.load())
    return;

  const uint32_t siteRva = g_mode66SiteRva.load();
  const uint32_t entries = g_drawWalkEntries.exchange(0);
  if (entries == 0xFFFFFFFFu) {
    Log("Mode66: COUNT hook died @exeRva=0x%X — wrote stereo=45 (safe default)", siteRva);
    Mode34ClearProbe("count died");
    if (g_drawWalkAddr) {
      MH_DisableHook(g_drawWalkAddr);
      MH_RemoveHook(g_drawWalkAddr);
      g_origDrawWalk = nullptr;
      g_drawWalkAddr = nullptr;
    }
    g_mode66CountDone.store(true);
    WriteStereoModeFile(45);
    return;
  }

  ++g_mode66CountEs;
  g_mode66EntrySum += entries;
  const uint32_t n = g_mode66CountEs.load();
  if (n <= 6 || (n % 15) == 0)
    Log("Mode66: COUNT es#%u/%u entries=%u sum=%u exeRva=0x%X", n, kMode66CountEsNeed, entries,
        g_mode66EntrySum, siteRva);

  if (n < kMode66CountEsNeed)
    return;

  const float avg = static_cast<float>(g_mode66EntrySum) / static_cast<float>(n);
  Mode34ClearProbe("COUNT done");
  if (g_drawWalkAddr) {
    MH_DisableHook(g_drawWalkAddr);
    MH_RemoveHook(g_drawWalkAddr);
    g_origDrawWalk = nullptr;
    g_drawWalkAddr = nullptr;
  }
  g_mode66CountDone.store(true);
  // PublishSync is shared (12 E8 + helper 0x31940×58). High avg = SHARED, not failure.
  const char* cadence = "REJECT";
  if (avg >= 0.8f && avg <= 4.f)
    cadence = "PASS";
  else if (avg > 4.f)
    cadence = "SHARED";
  Log("Mode66: COUNT done exeRva=0x%X avgEntries=%.2f cadence=%s "
      "(PASS=[0.8,4] owner-like; SHARED=>4 shared publish; REJECT<0.8) "
      "SameFrameSeamGate=CLOSED dual=OFF",
      siteRva, avg, cadence);
  if (cadence[0] == 'P' || cadence[0] == 'S')
    Log("Mode66: next live step — stereo=67 ViewMatWriter COUNT then 65/41/42");
  else
    Log("Mode66: cadence REJECT — PublishSync barely entered; keep stereo=45");
  WriteStereoModeFile(45);
}

bool Mode67InstallCountHook() {
  const ReSiteSpec spec{ "ViewMatWriter", kMode67Pattern, kMode67ViewMatExpectedRva,
                         kMode67Prologue, sizeof(kMode67Prologue), /*requireCcPad=*/true };
  const uint32_t siteRva = ResolveReSite(spec);
  g_mode67SiteRva.store(siteRva);
  if (!siteRva) {
    Log("Mode67: REJECT — ResolveReSite failed (expected mapped RVA 0x%X)",
        kMode67ViewMatExpectedRva);
    return false;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t start = base + siteRva;
  if (Mode34ForbiddenRva(siteRva)) {
    Log("Mode67: REJECT exeRva=0x%X forbidden", siteRva);
    return false;
  }
  const char* tag = "?";
  if (Mode32ClassifyPrologue(start, &tag) != Mode32Abi::Thiscall) {
    Log("Mode67: REJECT exeRva=0x%X abi=%s (need thiscall ViewMatWriter)", siteRva, tag);
    return false;
  }
  Mode34WriteProbe(siteRva);
  if (!InstallDrawWalkAt(start)) {
    Mode34ClearProbe("MH fail");
    Log("Mode67: InstallDrawWalkAt failed exeRva=0x%X", siteRva);
    return false;
  }
  g_mode67Es.store(0);
  g_mode67CountEs.store(0);
  g_mode67EntrySum = 0;
  g_mode67CountDone.store(false);
  g_drawWalkEntries.store(0);
  Log("Mode67: COUNT armed exeRva=0x%X abi=%s (ViewMatWriter mapped; want avg in [0.8,4] "
      "over %u EndScenes; dual=OFF SameFrameSeamGate=CLOSED)",
      siteRva, tag, kMode67CountEsNeed);
  return true;
}

void Mode67OnEndScene(IDirect3DDevice9* device) {
  TemporalCapturePairHold(device);
  if (g_mode67CountDone.load())
    return;

  const uint32_t siteRva = g_mode67SiteRva.load();
  const uint32_t entries = g_drawWalkEntries.exchange(0);
  if (entries == 0xFFFFFFFFu) {
    Log("Mode67: COUNT hook died @exeRva=0x%X — wrote stereo=45 (safe default)", siteRva);
    Mode34ClearProbe("count died");
    if (g_drawWalkAddr) {
      MH_DisableHook(g_drawWalkAddr);
      MH_RemoveHook(g_drawWalkAddr);
      g_origDrawWalk = nullptr;
      g_drawWalkAddr = nullptr;
    }
    g_mode67CountDone.store(true);
    WriteStereoModeFile(45);
    return;
  }

  ++g_mode67CountEs;
  g_mode67EntrySum += entries;
  const uint32_t n = g_mode67CountEs.load();
  if (n <= 6 || (n % 15) == 0)
    Log("Mode67: COUNT es#%u/%u entries=%u sum=%u exeRva=0x%X", n, kMode67CountEsNeed, entries,
        g_mode67EntrySum, siteRva);

  if (n < kMode67CountEsNeed)
    return;

  const float avg = static_cast<float>(g_mode67EntrySum) / static_cast<float>(n);
  Mode34ClearProbe("COUNT done");
  if (g_drawWalkAddr) {
    MH_DisableHook(g_drawWalkAddr);
    MH_RemoveHook(g_drawWalkAddr);
    g_origDrawWalk = nullptr;
    g_drawWalkAddr = nullptr;
  }
  g_mode67CountDone.store(true);
  // ViewMat has 9 E8 callers + dirty-flag rebuilds — avg>4 = SHARED, not failure.
  const char* cadence = "REJECT";
  if (avg >= 0.8f && avg <= 4.f)
    cadence = "PASS";
  else if (avg > 4.f)
    cadence = "SHARED";
  Log("Mode67: COUNT done exeRva=0x%X avgEntries=%.2f cadence=%s "
      "(PASS=[0.8,4]; SHARED=>4 dirty rebuilds; REJECT<0.8; ViewMatWriter) "
      "SameFrameSeamGate=CLOSED dual=OFF",
      siteRva, avg, cadence);
  if (cadence[0] == 'P' || cadence[0] == 'S')
    Log("Mode67: next live step — stereo=65 then 41/42; SameFrameSeamGate stays CLOSED");
  else
    Log("Mode67: cadence REJECT — ViewMatWriter barely entered; keep stereo=45");
  WriteStereoModeFile(45);
}

bool Mode68InstallCountHook() {
  // ReplayDispatch is thiscall without classic frame (cmp [gate]; push esi; mov esi,ecx).
  // requireCcPad=false — mid-function neighbors / no INT3 pad before next symbol.
  const ReSiteSpec spec{"ReplayDispatch", kMode68Pattern, kMode68ReplayDispatchExpectedRva,
                        kMode68Prologue, sizeof(kMode68Prologue), /*requireCcPad=*/false};
  const uint32_t siteRva = ResolveReSite(spec);
  g_mode68SiteRva.store(siteRva);
  if (!siteRva) {
    Log("Mode68: REJECT — ResolveReSite failed (expected mapped RVA 0x%X)",
        kMode68ReplayDispatchExpectedRva);
    return false;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t start = base + siteRva;
  if (Mode34ForbiddenRva(siteRva)) {
    Log("Mode68: REJECT exeRva=0x%X forbidden", siteRva);
    return false;
  }
  // Byte gate only — Mode32ClassifyPrologue wants 55 8B EC; ReplayDispatch is 83 3D…56 8B F1.
  if (std::memcmp(reinterpret_cast<const void*>(start), kMode68Prologue,
                  sizeof(kMode68Prologue)) != 0) {
    Log("Mode68: REJECT exeRva=0x%X prologue mismatch (need ReplayDispatch cmp/gate)", siteRva);
    return false;
  }
  Mode34WriteProbe(siteRva);
  if (!InstallDrawWalkAt(start)) {
    Mode34ClearProbe("MH fail");
    Log("Mode68: InstallDrawWalkAt failed exeRva=0x%X", siteRva);
    return false;
  }
  g_mode68Es.store(0);
  g_mode68CountEs.store(0);
  g_mode68EntrySum = 0;
  g_mode68CountDone.store(false);
  g_drawWalkEntries.store(0);
  Log("Mode68: COUNT armed exeRva=0x%X abi=thiscall-esi (ReplayDispatch→slot178 0x220D0; "
      "want avg in [0.8,4] over %u EndScenes; dual=OFF SameFrameSeamGate=CLOSED)",
      siteRva, kMode68CountEsNeed);
  return true;
}

void Mode68OnEndScene(IDirect3DDevice9* device) {
  TemporalCapturePairHold(device);
  if (g_mode68CountDone.load())
    return;

  const uint32_t siteRva = g_mode68SiteRva.load();
  const uint32_t entries = g_drawWalkEntries.exchange(0);
  if (entries == 0xFFFFFFFFu) {
    Log("Mode68: COUNT hook died @exeRva=0x%X — wrote stereo=45 (safe default)", siteRva);
    Mode34ClearProbe("count died");
    if (g_drawWalkAddr) {
      MH_DisableHook(g_drawWalkAddr);
      MH_RemoveHook(g_drawWalkAddr);
      g_origDrawWalk = nullptr;
      g_drawWalkAddr = nullptr;
    }
    g_mode68CountDone.store(true);
    WriteStereoModeFile(45);
    return;
  }

  ++g_mode68CountEs;
  g_mode68EntrySum += entries;
  const uint32_t n = g_mode68CountEs.load();
  if (n <= 6 || (n % 15) == 0)
    Log("Mode68: COUNT es#%u/%u entries=%u sum=%u exeRva=0x%X", n, kMode68CountEsNeed, entries,
        g_mode68EntrySum, siteRva);

  if (n < kMode68CountEsNeed)
    return;

  const float avg = static_cast<float>(g_mode68EntrySum) / static_cast<float>(n);
  Mode34ClearProbe("COUNT done");
  if (g_drawWalkAddr) {
    MH_DisableHook(g_drawWalkAddr);
    MH_RemoveHook(g_drawWalkAddr);
    g_origDrawWalk = nullptr;
    g_drawWalkAddr = nullptr;
  }
  g_mode68CountDone.store(true);
  // Exactly 2 E8 callers — owner-like cadence expected in [0.8,4] if view active.
  const char* cadence = "REJECT";
  if (avg >= 0.8f && avg <= 4.f)
    cadence = "PASS";
  else if (avg > 4.f)
    cadence = "SHARED";
  Log("Mode68: COUNT done exeRva=0x%X avgEntries=%.2f cadence=%s "
      "(PASS=[0.8,4] owner-like ReplayDispatch; SHARED=>4; REJECT<0.8; "
      "bridge to slot178=0x220D0) SameFrameSeamGate=CLOSED dual=OFF",
      siteRva, avg, cadence);
  if (cadence[0] == 'P')
    Log("Mode68: PASS — candidate seam for future dual BEFORE call [eax+0x178]; "
        "keep SameFrameSeamGate=CLOSED until dual plan reviewed; kill=45");
  else if (cadence[0] == 'S')
    Log("Mode68: SHARED — more entries than 2-caller model; inspect callers before dual; kill=45");
  else
    Log("Mode68: cadence REJECT — ReplayDispatch barely entered; keep stereo=45");
  WriteStereoModeFile(45);
}

bool Mode69InstallPeekHook() {
  const ReSiteSpec spec{"ReplayDispatch", kMode69Pattern, kMode69ReplayDispatchExpectedRva,
                        kMode69Prologue, sizeof(kMode69Prologue), /*requireCcPad=*/false};
  const uint32_t siteRva = ResolveReSite(spec);
  g_mode69SiteRva.store(siteRva);
  if (!siteRva) {
    Log("Mode69: REJECT — ResolveReSite failed (expected mapped RVA 0x%X)",
        kMode69ReplayDispatchExpectedRva);
    return false;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t start = base + siteRva;
  if (Mode34ForbiddenRva(siteRva)) {
    Log("Mode69: REJECT exeRva=0x%X forbidden", siteRva);
    return false;
  }
  if (std::memcmp(reinterpret_cast<const void*>(start), kMode69Prologue,
                  sizeof(kMode69Prologue)) != 0) {
    Log("Mode69: REJECT exeRva=0x%X prologue mismatch", siteRva);
    return false;
  }
  Mode34WriteProbe(siteRva);
  if (!InstallDrawWalkAt(start)) {
    Mode34ClearProbe("MH fail");
    Log("Mode69: InstallDrawWalkAt failed exeRva=0x%X", siteRva);
    return false;
  }
  g_mode69Es.store(0);
  g_mode69PeekEs.store(0);
  g_mode69WaitEs.store(0);
  g_mode69Live.store(false);
  g_mode69PeekDone.store(false);
  g_mode69PeekOk.store(0);
  g_mode69PeekFail.store(0);
  g_mode69LogLeft.store(8);
  g_mode69HaveLast = false;
  g_mode69MaxDelta = 0.f;
  g_drawWalkEntries.store(0);
  Log("Mode69: PEEK armed exeRva=0x%X (WAIT for non-identity view+0x80, then %u ES; "
      "waitCap=%u; dual=OFF SameFrameSeamGate=CLOSED)",
      siteRva, kMode69PeekEsNeed, kMode69WaitEsMax);
  return true;
}

void Mode69OnEndScene(IDirect3DDevice9* device) {
  TemporalCapturePairHold(device);
  if (g_mode69PeekDone.load())
    return;

  const uint32_t siteRva = g_mode69SiteRva.load();
  const uint32_t entries = g_drawWalkEntries.exchange(0);
  if (entries == 0xFFFFFFFFu) {
    Log("Mode69: PEEK hook died @exeRva=0x%X — wrote stereo=45 (safe default)", siteRva);
    Mode34ClearProbe("peek died");
    if (g_drawWalkAddr) {
      MH_DisableHook(g_drawWalkAddr);
      MH_RemoveHook(g_drawWalkAddr);
      g_origDrawWalk = nullptr;
      g_drawWalkAddr = nullptr;
    }
    g_mode69PeekDone.store(true);
    WriteStereoModeFile(45);
    return;
  }

  if (!g_mode69Live.load()) {
    ++g_mode69WaitEs;
    const uint32_t w = g_mode69WaitEs.load();
    if (w <= 6 || (w % 60) == 0)
      Log("Mode69: WAIT es#%u/%u entries=%u ok=%u (need non-identity — load into world)",
          w, kMode69WaitEsMax, entries, g_mode69PeekOk.load());
    if (w < kMode69WaitEsMax)
      return;
    Mode34ClearProbe("WAIT timeout");
    if (g_drawWalkAddr) {
      MH_DisableHook(g_drawWalkAddr);
      MH_RemoveHook(g_drawWalkAddr);
      g_origDrawWalk = nullptr;
      g_drawWalkAddr = nullptr;
    }
    g_mode69PeekDone.store(true);
    Log("Mode69: PEEK done exeRva=0x%X cadence=REJECT ok=%u fail=%u "
        "(only identity at view+0x80 during wait) dual=OFF",
        siteRva, g_mode69PeekOk.load(), g_mode69PeekFail.load());
    WriteStereoModeFile(45);
    return;
  }

  ++g_mode69PeekEs;
  const uint32_t n = g_mode69PeekEs.load();
  if (n <= 6 || (n % 15) == 0)
    Log("Mode69: PEEK es#%u/%u entries=%u ok=%u fail=%u maxDelta2=%.6f exeRva=0x%X", n,
        kMode69PeekEsNeed, entries, g_mode69PeekOk.load(), g_mode69PeekFail.load(),
        g_mode69MaxDelta, siteRva);

  if (n < kMode69PeekEsNeed)
    return;

  Mode34ClearProbe("PEEK done");
  if (g_drawWalkAddr) {
    MH_DisableHook(g_drawWalkAddr);
    MH_RemoveHook(g_drawWalkAddr);
    g_origDrawWalk = nullptr;
    g_drawWalkAddr = nullptr;
  }
  g_mode69PeekDone.store(true);
  const uint32_t ok = g_mode69PeekOk.load();
  const uint32_t fail = g_mode69PeekFail.load();
  const char* cadence = "REJECT";
  if (ok >= 20 && fail == 0 && g_mode69MaxDelta > 1e-8f)
    cadence = "PASS";
  else if (ok >= 20 && fail == 0 && g_mode69Live.load())
    cadence = "STATIC";
  else if (ok >= 20)
    cadence = "PARTIAL";
  Log("Mode69: PEEK done exeRva=0x%X cadence=%s ok=%u fail=%u maxDelta2=%.6f "
      "(PASS=live+changing; STATIC=live but frozen; REJECT=no live) dual=OFF",
      siteRva, cadence, ok, fail, g_mode69MaxDelta);
  if (g_mode69HaveLast)
    Log("Mode69: last t=(%.3f,%.3f,%.3f) — inject L/R here before call @0x30D0D (not yet)",
        g_mode69LastMat[12], g_mode69LastMat[13], g_mode69LastMat[14]);
  // Continuous test: chain to Mode 70 (activeView filter). Hook-death still writes 45.
  WriteStereoModeFile(70);
}

bool Mode70InstallPeekHook() {
  const ReSiteSpec spec{"ReplayDispatch", kMode70Pattern, kMode70ReplayDispatchExpectedRva,
                        kMode70Prologue, sizeof(kMode70Prologue), /*requireCcPad=*/false};
  const uint32_t siteRva = ResolveReSite(spec);
  g_mode70SiteRva.store(siteRva);
  if (!siteRva) {
    Log("Mode70: REJECT — ResolveReSite failed (expected mapped RVA 0x%X)",
        kMode70ReplayDispatchExpectedRva);
    return false;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t start = base + siteRva;
  if (Mode34ForbiddenRva(siteRva)) {
    Log("Mode70: REJECT exeRva=0x%X forbidden", siteRva);
    return false;
  }
  if (std::memcmp(reinterpret_cast<const void*>(start), kMode70Prologue,
                  sizeof(kMode70Prologue)) != 0) {
    Log("Mode70: REJECT exeRva=0x%X prologue mismatch", siteRva);
    return false;
  }
  Mode34WriteProbe(siteRva);
  if (!InstallDrawWalkAt(start)) {
    Mode34ClearProbe("MH fail");
    Log("Mode70: InstallDrawWalkAt failed exeRva=0x%X", siteRva);
    return false;
  }
  g_mode70WaitEs.store(0);
  g_mode70PeekEs.store(0);
  g_mode70Live.store(false);
  g_mode70PeekDone.store(false);
  g_mode70LiveTotal.store(0);
  g_mode70ActiveHit.store(0);
  g_mode70ActiveMiss.store(0);
  g_mode70LogLeft.store(8);
  g_mode70HaveLast = false;
  g_mode70MaxDelta = 0.f;
  g_drawWalkEntries.store(0);
  Log("Mode70: PEEK armed exeRva=0x%X (WAIT activeView+live, then %u ES; waitCap=%u; "
      "dual=OFF; success keeps stereo=70)",
      siteRva, kMode70PeekEsNeed, kMode70WaitEsMax);
  return true;
}

void Mode70OnEndScene(IDirect3DDevice9* device) {
  TemporalCapturePairHold(device);
  if (g_mode70PeekDone.load())
    return;

  const uint32_t siteRva = g_mode70SiteRva.load();
  const uint32_t entries = g_drawWalkEntries.exchange(0);
  if (entries == 0xFFFFFFFFu) {
    Log("Mode70: PEEK hook died @exeRva=0x%X — wrote stereo=45 (safe default)", siteRva);
    Mode34ClearProbe("peek died");
    if (g_drawWalkAddr) {
      MH_DisableHook(g_drawWalkAddr);
      MH_RemoveHook(g_drawWalkAddr);
      g_origDrawWalk = nullptr;
      g_drawWalkAddr = nullptr;
    }
    g_mode70PeekDone.store(true);
    WriteStereoModeFile(45);
    return;
  }

  if (!g_mode70Live.load()) {
    ++g_mode70WaitEs;
    const uint32_t w = g_mode70WaitEs.load();
    if (w <= 6 || (w % 60) == 0)
      Log("Mode70: WAIT es#%u/%u entries=%u liveTot=%u hit=%u miss=%u "
          "(need activeView+live — load into world)",
          w, kMode70WaitEsMax, entries, g_mode70LiveTotal.load(), g_mode70ActiveHit.load(),
          g_mode70ActiveMiss.load());
    if (w < kMode70WaitEsMax)
      return;
    Mode34ClearProbe("WAIT timeout");
    if (g_drawWalkAddr) {
      MH_DisableHook(g_drawWalkAddr);
      MH_RemoveHook(g_drawWalkAddr);
      g_origDrawWalk = nullptr;
      g_drawWalkAddr = nullptr;
    }
    g_mode70PeekDone.store(true);
    Log("Mode70: PEEK done exeRva=0x%X cadence=REJECT liveTot=%u hit=%u miss=%u "
        "(no activeView+live during wait) dual=OFF",
        siteRva, g_mode70LiveTotal.load(), g_mode70ActiveHit.load(),
        g_mode70ActiveMiss.load());
    // Keep stereo=70 for retry after load — user continuous test.
    return;
  }

  ++g_mode70PeekEs;
  const uint32_t n = g_mode70PeekEs.load();
  if (n <= 6 || (n % 15) == 0)
    Log("Mode70: PEEK es#%u/%u entries=%u hit=%u miss=%u maxDelta2=%.6f exeRva=0x%X", n,
        kMode70PeekEsNeed, entries, g_mode70ActiveHit.load(), g_mode70ActiveMiss.load(),
        g_mode70MaxDelta, siteRva);

  if (n < kMode70PeekEsNeed)
    return;

  Mode34ClearProbe("PEEK done");
  if (g_drawWalkAddr) {
    MH_DisableHook(g_drawWalkAddr);
    MH_RemoveHook(g_drawWalkAddr);
    g_origDrawWalk = nullptr;
    g_drawWalkAddr = nullptr;
  }
  g_mode70PeekDone.store(true);
  const uint32_t hit = g_mode70ActiveHit.load();
  const uint32_t miss = g_mode70ActiveMiss.load();
  const uint32_t live = g_mode70LiveTotal.load();
  const float hitRate = live ? (100.f * static_cast<float>(hit) / static_cast<float>(live)) : 0.f;
  const char* cadence = "REJECT";
  if (hit >= 20 && g_mode70MaxDelta > 1e-8f)
    cadence = "PASS";
  else if (hit >= 20)
    cadence = "STATIC";
  else if (hit > 0)
    cadence = "PARTIAL";
  Log("Mode70: PEEK done exeRva=0x%X cadence=%s hit=%u miss=%u liveTot=%u hitRate=%.1f%% "
      "maxDelta2=%.6f (PASS=activeView live+changing) dual=OFF SameFrameSeamGate=CLOSED",
      siteRva, cadence, hit, miss, live, hitRate, g_mode70MaxDelta);
  if (g_mode70HaveLast)
    Log("Mode70: last t=(%.3f,%.3f,%.3f) — next: dual inject only when self==activeView",
        g_mode70LastMat[12], g_mode70LastMat[13], g_mode70LastMat[14]);
  // Continuous test: do NOT force stereo=45 on success.
}

bool Mode71InstallInjectHook() {
  // Hard-disable: in-place write to activeView+0x80 froze the game twice (safe gates
  // were not enough). Keep symbol so stereo=71 remaps cleanly; never MinHook for 71.
  Log("Mode71: DISABLED — in-place view+0x80 inject freezes (2026-07-25); use stereo=45; "
      "next inject path must not mutate live view object");
  g_mode71Armed.store(false);
  g_mode71Dead.store(false);
  return false;
}

void Mode71OnEndScene(IDirect3DDevice9* device) {
  TemporalCapturePairHold(device);
  // Next frame may inject once with the eye we just latched.
  g_mode71Budget.store(1);
  if (g_mode71Dead.load()) {
    Mode34ClearProbe("inject died");
    if (g_drawWalkAddr) {
      MH_DisableHook(g_drawWalkAddr);
      MH_RemoveHook(g_drawWalkAddr);
      g_origDrawWalk = nullptr;
      g_drawWalkAddr = nullptr;
    }
    g_mode71Armed.store(false);
    WriteStereoModeFile(45);
    return;
  }
  const uint32_t entries = g_drawWalkEntries.exchange(0);
  if (entries == 0xFFFFFFFFu) {
    g_mode71Dead.store(true);
    return;
  }
  static uint32_t s_es = 0;
  ++s_es;
  if (s_es <= 6 || (s_es % 120) == 0)
    Log("Mode71: es#%u entries=%u injects=%u skip=%u rejectMat=%u budget=%u sep=%.0fcm "
        "scale=%.2f eye=%d (SAFE retest; kill=45)",
        s_es, entries, g_mode71Injects.load(), g_mode71Skip.load(), g_mode71RejectMat.load(),
        g_mode71Budget.load(), GetStereoSepMeters() * 100.f, GetStereoScale(),
        static_cast<int>(GetStereoEye()));
}

bool Mode72InstallHook() {
  const ReSiteSpec spec{"SetVSConstF", kMode72Pattern, kMode72SetVSConstExpectedRva,
                        kMode72Prologue, sizeof(kMode72Prologue), /*requireCcPad=*/false};
  const uint32_t siteRva = ResolveReSite(spec);
  g_mode72SiteRva.store(siteRva);
  if (!siteRva) {
    Log("Mode72: REJECT — ResolveReSite failed (expected mapped RVA 0x%X)",
        kMode72SetVSConstExpectedRva);
    return false;
  }
  if (Mode34ForbiddenRva(siteRva)) {
    Log("Mode72: REJECT exeRva=0x%X forbidden", siteRva);
    return false;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t start = base + siteRva;
  if (std::memcmp(reinterpret_cast<const void*>(start), kMode72Prologue,
                  sizeof(kMode72Prologue)) != 0) {
    Log("Mode72: REJECT exeRva=0x%X prologue mismatch", siteRva);
    return false;
  }
  if (g_mode72HookAddr) {
    MH_DisableHook(g_mode72HookAddr);
    MH_RemoveHook(g_mode72HookAddr);
    g_mode72HookAddr = nullptr;
    g_origMode72SetVS = nullptr;
  }
  Mode34WriteProbe(siteRva);
  if (MH_CreateHook(reinterpret_cast<void*>(start), reinterpret_cast<void*>(&HookMode72SetVSConstF),
                    reinterpret_cast<void**>(&g_origMode72SetVS)) != MH_OK ||
      MH_EnableHook(reinterpret_cast<void*>(start)) != MH_OK) {
    Mode34ClearProbe("MH fail");
    g_origMode72SetVS = nullptr;
    Log("Mode72: MH_CreateHook failed exeRva=0x%X", siteRva);
    return false;
  }
  g_mode72HookAddr = reinterpret_cast<void*>(start);
  g_mode72Armed.store(true);
  g_mode72Dead.store(false);
  g_mode72Injects.store(0);
  g_mode72Skip.store(0);
  g_mode72RejectMat.store(0);
  g_mode72Budget.store(1);
  g_mode74ProjInjects.store(0);
  g_mode74ProjDevicePatches.store(0);
  g_mode74ProjMiss.store(0);
  g_mode74ProjDiscoverN.store(0);
  g_mode74ProjCacheOk.store(false);
  Log("Mode72: INJECT armed exeRva=0x%X (SetVSConstF src-COPY; never writes view+0x80; "
      "1/ES; unit-right; motion-guard OFF; halfSep floor 5cm for visibility; "
      "SameFrameSeamGate=CLOSED — still TEMPORAL not true dual)",
      siteRva);
  return true;
}

bool Mode74InstallPublishProjHook() {
  if (g_mode74PublishProjArmed.load())
    return true;
  if (Mode34ForbiddenRva(kMode74PublishProjRva)) {
    Log("PubProj: REJECT exeRva=0x%X forbidden", kMode74PublishProjRva);
    return false;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t start = base + kMode74PublishProjRva;
  if (std::memcmp(reinterpret_cast<const void*>(start), kMode74PublishProjPrologue,
                  sizeof(kMode74PublishProjPrologue)) != 0) {
    Log("PubProj: REJECT exeRva=0x%X prologue mismatch (need COUNT/RE first)",
        kMode74PublishProjRva);
    return false;
  }
  if (g_mode74PublishProjHookAddr) {
    MH_DisableHook(g_mode74PublishProjHookAddr);
    MH_RemoveHook(g_mode74PublishProjHookAddr);
    g_mode74PublishProjHookAddr = nullptr;
    g_origPublishProj = nullptr;
  }
  if (MH_CreateHook(reinterpret_cast<void*>(start),
                    reinterpret_cast<void*>(&HookMode74PublishProj),
                    reinterpret_cast<void**>(&g_origPublishProj)) != MH_OK ||
      MH_EnableHook(reinterpret_cast<void*>(start)) != MH_OK) {
    g_origPublishProj = nullptr;
    Log("PubProj: MH_CreateHook failed exeRva=0x%X", kMode74PublishProjRva);
    return false;
  }
  g_mode74PublishProjHookAddr = reinterpret_cast<void*>(start);
  g_mode74PublishProjArmed.store(true);
  g_mode74PublishProjCalls.store(0);
  g_mode74PublishProjInjects.store(0);
  g_mode74PubC0Writes.store(0);
  Log("PubProj: ARMED exeRva=0x%X (TEMPORAL nearCam/active → +0x308 HMD FOV; dual HOLD "
      "+0x180 for eye walk; never view+0x80)",
      kMode74PublishProjRva);
  return true;
}

bool Mode74InstallUploadFnHook() {
  if (g_mode74UploadFnArmed.load())
    return true;
#if !(defined(_MSC_VER) && defined(_M_IX86))
  Log("UploadFn: REJECT — need MSVC x86 naked thiscall trampoline");
  return false;
#else
  if (Mode34ForbiddenRva(kMode74UploadFnRva)) {
    Log("UploadFn: REJECT exeRva=0x%X forbidden", kMode74UploadFnRva);
    return false;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const uintptr_t start = base + kMode74UploadFnRva;
  if (std::memcmp(reinterpret_cast<const void*>(start), kMode74UploadFnPrologue,
                  sizeof(kMode74UploadFnPrologue)) != 0) {
    Log("UploadFn: REJECT exeRva=0x%X prologue mismatch", kMode74UploadFnRva);
    return false;
  }
  if (g_mode74UploadFnHookAddr) {
    MH_DisableHook(g_mode74UploadFnHookAddr);
    MH_RemoveHook(g_mode74UploadFnHookAddr);
    g_mode74UploadFnHookAddr = nullptr;
    g_origUploadFn = nullptr;
  }
  if (MH_CreateHook(reinterpret_cast<void*>(start),
                    reinterpret_cast<void*>(&HookMode74UploadFnNaked),
                    reinterpret_cast<void**>(&g_origUploadFn)) != MH_OK ||
      MH_EnableHook(reinterpret_cast<void*>(start)) != MH_OK) {
    g_origUploadFn = nullptr;
    Log("UploadFn: MH_CreateHook failed exeRva=0x%X", kMode74UploadFnRva);
    return false;
  }
  g_mode74UploadFnHookAddr = reinterpret_cast<void*>(start);
  g_mode74UploadFnArmed.store(true);
  g_mode74UploadFnDead.store(false);
  g_mode74UploadFnCalls.store(0);
  g_mode74UploadFnEyeWrites.store(0);
  g_mode74UploadFnOwnsEye.store(false);
  Log("UploadFn: ARMED exeRva=0x%X (vtable MatMul×3→+178; bak±IPD+toeIn on +0x80 for "
      "call; ALWAYS restore; Mode72 skips while OwnsEye; CamMatrix IPD off; dual still OFF)",
      kMode74UploadFnRva);
  return true;
#endif
}

void Mode72OnEndScene(IDirect3DDevice9* device) {
  TemporalCapturePairHold(device);
  g_mode72Budget.store(1);
  if (g_mode72Dead.load()) {
    Mode34ClearProbe("Mode72 dead");
    if (g_mode72HookAddr) {
      MH_DisableHook(g_mode72HookAddr);
      MH_RemoveHook(g_mode72HookAddr);
      g_mode72HookAddr = nullptr;
      g_origMode72SetVS = nullptr;
    }
    g_mode72Armed.store(false);
    WriteStereoModeFile(45);
    Log("Mode72: wrote stereo=45 after fault");
    return;
  }
  static uint32_t s_es = 0;
  ++s_es;
  if (s_es <= 6 || (s_es % 120) == 0)
    Log("Mode72: es#%u injects=%u skip=%u rejectMat=%u sepFile=%.0fcm "
        "scale=%.2f eye=%d motionGuard=OFF (temporal L≠R; F8 1cm=most fused; kill=45)",
        s_es, g_mode72Injects.load(), g_mode72Skip.load(), g_mode72RejectMat.load(),
        GetStereoSepMeters() * 100.f, GetStereoScale(), static_cast<int>(GetStereoEye()));
}

void Mode74BeginHardOff(const char* reason) {
  g_mode74LiveStreak.store(0);
  g_mode74MissStreak.store(0);
  g_mode74WorldWeakStreak.store(0);
  g_mode74SoftSkipStreak.store(0);
  g_mode74DualSkipRemain.store(0);
  g_mode74GateOpen.store(false);
  g_mode74DidDualThisFrame.store(false);
  // CRITICAL: dualEn=0 so BuildRootA×2 cannot run; dualN must freeze in logs.
  g_mode74DualEnabled.store(false);
  // Session latch: do NOT reopen after cool-down (was thrashing stuttery↔smooth).
  const bool firstLatch = !g_mode74DualSessionOff.exchange(true);
  if (firstLatch) {
    Log("Mode74: DUAL HARD OFF (%s) — dualEn=0 dualN=%u frozen", reason,
        g_mode74DualN.load());
    Log("Mode75: DUAL SESSION OFF — dual stays off this process (no reopen thrash); "
        "pair-hold temporal kept; retry = stereo=75 + dual=force Quick Restart; kill=45");
  } else {
    Log("Mode74: DUAL HARD OFF (%s) — already SESSION OFF dualEn=0 dualN=%u", reason,
        g_mode74DualN.load());
  }
}

bool Mode74BuildDualFilePath(char* path, size_t pathChars) {
  HMODULE self = nullptr;
  GetModuleHandleExA(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCSTR>(&Mode74BuildDualFilePath), &self);
  if (!GetModuleFileNameA(self, path, static_cast<DWORD>(pathChars)))
    return false;
  char* slash = strrchr(path, '\\');
  if (!slash)
    slash = strrchr(path, '/');
  if (slash)
    *(slash + 1) = '\0';
  else
    path[0] = '\0';
  return strcat_s(path, pathChars, "gtaiv_dxvk_vr.dual") == 0;
}

bool Mode74ReadDualOptInFile() {
  // Default OFF (smooth load / city). Only explicit "1" or "force" enables sparse dual.
  char path[MAX_PATH]{};
  if (!Mode74BuildDualFilePath(path, MAX_PATH))
    return false;
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return false;
  char buf[8]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0)
    return false;
  return buf[0] == '1' || buf[0] == 'f' || buf[0] == 'F';
}

// Mid-session: dual file "force" → enable opt-in + clear SESSION OFF once.
// Consumes force by rewriting dual file to "1" so hitch→latch cannot loop forever.
bool Mode74PollDualForceRetry() {
  char path[MAX_PATH]{};
  if (!Mode74BuildDualFilePath(path, MAX_PATH))
    return false;
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return false;
  char buf[16]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0)
    return false;
  const bool force = (buf[0] == 'f' || buf[0] == 'F');
  if (!force)
    return false;
  // Already opt-in and not latched — nothing to do (avoid rewrite spam).
  if (g_mode74DualOptIn.load() && !g_mode74DualSessionOff.load())
    return false;
  g_mode74DualSessionOff.store(false);
  g_mode74DualEnabled.store(false);
  g_mode74GateOpen.store(false);
  g_mode74LiveStreak.store(0);
  g_mode74MissStreak.store(0);
  g_mode74DualN.store(0);
  g_mode74DualOptIn.store(true);
  FILE* wf = nullptr;
  if (fopen_s(&wf, path, "wb") == 0 && wf) {
    fputc('1', wf);
    fclose(wf);
  }
  Log("Mode74: DUAL FORCE ON (file=force→1) — will arm after %u live ES; "
      "HARD OFF → SESSION OFF; kill=45",
      kMode74GateNeedLiveEs);
  return true;
}

void Mode74UpdateStreamingGate() {
  static float s_prevX = 0.f, s_prevY = 0.f, s_prevZ = 0.f;
  static bool s_havePrevCam = false;
  static DWORD s_prevTick = 0;

  // Dual path is Mode 75 only — never arm from dual file on hitchcut(74)/AER(76).
  if (!IsSparseSessionDual(GetStereoMode())) {
    g_mode74DualOptIn.store(false);
    g_mode74DualEnabled.store(false);
    g_mode74GateOpen.store(false);
    return;
  }

  const uint32_t inj = g_mode72Injects.load();
  const bool injectLive = inj > g_mode74LastInjects;
  // CRITICAL: dual must NEVER count as live for gate/miss — softskip failed because
  // dual reset miss every frame and dualN kept climbing through streaming.
  // Live = inject OK on a mono/temporal frame only.
  const bool liveThisEs = injectLive;
  g_mode74LastInjects = inj;

  const DWORD now = GetTickCount();
  float cx = 0.f, cy = 0.f, cz = 0.f;
  const bool haveCam = GetLastStereoCamPos(&cx, &cy, &cz);

  // Mid-session force retry via dual file ("force") — Mode 75 only.
  Mode74PollDualForceRetry();

  // File/permanent off / session latch — no dual; keep temporal motion.
  if (g_mode74DualDead.load() || !g_mode74DualOptIn.load() ||
      g_mode74DualSessionOff.load()) {
    if (g_mode74GateOpen.exchange(false))
      Log("Mode74: DUAL OFF — streaming gate CLOSED (dual disabled / dead / SESSION OFF)");
    g_mode74DualEnabled.store(false);
    g_mode74LiveStreak.store(0);
    g_mode74MissStreak.store(0);
    g_mode74WorldWeakStreak.store(0);
    if (haveCam) {
      s_prevX = cx;
      s_prevY = cy;
      s_prevZ = cz;
      s_havePrevCam = true;
    }
    s_prevTick = now;
    return;
  }

  // Load detectors while dual armed OR after first dual existed (catch apt→city).
  const bool dualArmed = g_mode74GateOpen.load() || g_mode74DualEnabled.load() ||
                         g_mode74DualN.load() > 0;
  if (dualArmed) {
    if (s_prevTick != 0) {
      const DWORD gap = now - s_prevTick;
      if (gap >= kMode74HitchMs) {
        Mode74BeginHardOff("EndScene hitch / streaming load");
        if (haveCam) {
          s_prevX = cx;
          s_prevY = cy;
          s_prevZ = cz;
          s_havePrevCam = true;
        }
        s_prevTick = now;
        return;
      }
      if (gap >= kMode74SoftSkipMs) {
        const uint32_t soft = g_mode74SoftSkipStreak.fetch_add(1) + 1;
        if (soft >= kMode74SoftSkipToPause) {
          Mode74BeginHardOff("slow-frame streak (streaming stress)");
          if (haveCam) {
            s_prevX = cx;
            s_prevY = cy;
            s_prevZ = cz;
            s_havePrevCam = true;
          }
          s_prevTick = now;
          return;
        }
      } else {
        g_mode74SoftSkipStreak.store(0);
      }
    }
    if (haveCam && s_havePrevCam) {
      const float dx = cx - s_prevX;
      const float dy = cy - s_prevY;
      const float dz = cz - s_prevZ;
      const float d2 = dx * dx + dy * dy + dz * dz;
      const float lim = kMode74TeleportMeters * kMode74TeleportMeters;
      if (d2 > lim) {
        Mode74BeginHardOff("cam teleport / interior-exterior");
        s_prevX = cx;
        s_prevY = cy;
        s_prevZ = cz;
        s_prevTick = now;
        return;
      }
    }
    float probe[16]{};
    if (!BuildLiveViewMatrix16(probe)) {
      const uint32_t weak = g_mode74WorldWeakStreak.fetch_add(1) + 1;
      if (weak >= kMode74UnloadNeedEs) {
        Mode74BeginHardOff("world cam missing (unload/stream)");
        if (haveCam) {
          s_prevX = cx;
          s_prevY = cy;
          s_prevZ = cz;
          s_havePrevCam = true;
        }
        s_prevTick = now;
        return;
      }
    } else {
      g_mode74WorldWeakStreak.store(0);
    }
  } else {
    g_mode74WorldWeakStreak.store(0);
  }

  if (haveCam) {
    s_prevX = cx;
    s_prevY = cy;
    s_prevZ = cz;
    s_havePrevCam = true;
  }
  s_prevTick = now;

  if (liveThisEs) {
    g_mode74MissStreak.store(0);
    const uint32_t streak = g_mode74LiveStreak.fetch_add(1) + 1;
    if (!g_mode74GateOpen.load() && streak >= kMode74GateNeedLiveEs) {
      g_mode74GateOpen.store(true);
      g_mode74DualEnabled.store(true);  // first arm only; HARD OFF → SESSION OFF
      g_mode74BuildTick.store(0);
      Log("Mode74: streaming gate OPEN after %u live EndScenes (sparse dual BuildRootA×2 "
          "1/%u; dualEn=1; HARD OFF → DUAL SESSION OFF (no reopen); hitch≥%ums; kill=45)",
          streak, kMode74DualEveryN, kMode74HitchMs);
    }
  } else {
    g_mode74LiveStreak.store(0);
    const uint32_t miss = g_mode74MissStreak.fetch_add(1) + 1;
    // Hysteresis: miss streak while OPEN → HARD OFF → SESSION OFF.
    if (g_mode74GateOpen.load() && miss >= kMode74MissCloseEs) {
      Mode74BeginHardOff("inject miss streak (likely load/streaming)");
      return;
    }
    if (!g_mode74GateOpen.load() && (miss <= 3 || (miss % 120) == 0)) {
      Log("Mode74: gate still CLOSED miss=%u dualEn=%d (need %u live ES before dual)", miss,
          g_mode74DualEnabled.load() ? 1 : 0, kMode74GateNeedLiveEs);
    }
  }
}

void Mode74OnEndScene(IDirect3DDevice9* device) {
  static DWORD s_prevEsTick = 0;
  const DWORD nowEs = GetTickCount();
  if (s_prevEsTick != 0)
    g_mode74LastEsMs.store(nowEs - s_prevEsTick);
  s_prevEsTick = nowEs;

  // Pose sample already taken in TryMonoSubmit (WaitGetPoses → BeginFrameHmdPoseSample).
  // Shared FrameDesc for cam glue (DRAW inject) + submit glue (TextureWithPose).
  Mode74SyncFrameDesc();
  // Cache HMD proj once/ES (PubProj temporal reuses — no BuildHmd per inject).
  Mode74CacheEyeProjections();

  // Prove drawn FOV via device VS projection scales BEFORE HOLD restore.
  // Cinema CCam45 → rendered V~58.7° → sy≈1/tan(29.35°)≈1.78.
  // Wide CCam~80 → rendered V~104° → sy≈0.9. If sy stays ~1.7, CCam is ignored.
  if (device && IsMode74Family(GetStereoMode())) {
    float bestSy = 99.f, bestSx = 0.f;
    int found = 0;
    for (UINT base = 0; base <= 48; base += 4) {
      float block[16]{};
      if (FAILED(device->GetVertexShaderConstantF(base, block, 4)))
        continue;
      if (!Mode74LooksLikeProjection(block))
        continue;
      ++found;
      // Lower sy = wider vertical FOV (cinema~1.78, HMD cover~0.95).
      if (block[5] < bestSy) {
        bestSy = block[5];
        bestSx = block[0];
      }
    }
    if (found == 0)
      bestSy = 0.f;
    // Wide FOV: sy drops below ~1.25 (cinema ~1.78). Require stable samples.
    static uint32_t s_wideHits = 0;
    static uint32_t s_cinemaHits = 0;
    static uint32_t s_proofGen = 0;
    const uint32_t proofGen = GetDrawnFovProofGeneration();
    if (proofGen != s_proofGen) {
      s_proofGen = proofGen;
      s_wideHits = 0;
      s_cinemaHits = 0;
    }
    if (found > 0 && bestSy > 0.2f && bestSy < 1.25f)
      ++s_wideHits;
    else if (found > 0 && bestSy >= 1.45f)
      ++s_cinemaHits;
    if (!IsDrawnFovProven() && s_wideHits >= 8) {
      SetDrawnFovProven(true);
      // Honest canvas from measured sy — Mode82 giant was CCam publish > drawn.
      PublishGameFovFromDrawnSy(bestSx, bestSy, GetBackbufferAspect());
      Log("FOVPROOF: DRAWN WIDE sy=%.3f sx=%.3f found=%d — canvas ungated (measured)", bestSy,
          bestSx, found);
    }
    // Mode79 ONLY — FOVPROOF spam on 80/83 was street hitch.
    if (IsFovPresenceHammer(GetStereoMode())) {
      static uint32_t s_meas = 0;
      ++s_meas;
      if (s_meas <= 8 || (s_meas % 120) == 0) {
        float gameH = 0.f, gameV = 0.f;
        GetGameFovTangents(&gameH, &gameV);
        float coverH = 0.f, coverV = 0.f;
        GetCoverFovTangents(&coverH, &coverV);
        float heldSx = -1.f;
        for (int i = 0; i < g_mode74HeldPubN; ++i) {
          if (!g_mode74HeldPub[i].held || !g_mode74HeldPub[i].self)
            continue;
          __try {
            auto* p180 = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(g_mode74HeldPub[i].self) + 0x180);
            heldSx = p180[0];
          } __except (EXCEPTION_EXECUTE_HANDLER) {
          }
          break;
        }
        float p180sx = 0.f, p308a = 0.f;
        bool usedCache = false;
        const uint32_t active = Mode74ResolveViewForInject(&usedCache);
        if (active) {
          __try {
            const auto* base = reinterpret_cast<const float*>(static_cast<uintptr_t>(active));
            p180sx = base[0x180 / 4];
            p308a = base[0x308 / 4];
          } __except (EXCEPTION_EXECUTE_HANDLER) {
          }
        }
        const uint32_t pn = ++g_mode79FovProofN;
        Log("Mode79: FOVPROOF #%u cover=(%.3f,%.3f) gameTan=(%.3f,%.3f) pendingCcam=%.1f "
            "proven=%d vsProj found=%d sx=%.3f sy=%.3f wideHits=%u cinemaHits=%u "
            "held+0x180[0]=%.3f active+0x180[0]=%.3f +0x308[0]=%.3f pubProj=%u eyeProj=%u "
            "devPatch=%u active=0x%X (gate canvas until sy<1.25; never view+0x80)",
            pn, coverH, coverV, gameH, gameV, GetPendingCcamFovDegrees(),
            IsDrawnFovProven() ? 1 : 0, found, bestSx, bestSy, s_wideHits, s_cinemaHits, heldSx,
            p180sx, p308a, g_mode74PublishProjInjects.load(), g_mode74ProjInjects.load(),
            g_mode74ProjDevicePatches.load(), active);
        if (s_cinemaHits > 60 && s_wideHits < 3 && (s_meas % 120) == 0)
          Log("FOVPROOF: CCam write NOT reaching drawn VS proj (sy still cinema~%.2f) — "
              "honest canvas stays gated; presence still blocked on unknown FOV owner",
              bestSy);
      }
    }
  }

  // Temporal PubProj HOLD ends here (dual restores after each eye walk).
  if (!g_inDual.load())
    Mode74RestoreHeldPub180();

  if (g_mode72Dead.load()) {
    Mode34ClearProbe("Mode74/72 dead");
    if (g_mode72HookAddr) {
      MH_DisableHook(g_mode72HookAddr);
      MH_RemoveHook(g_mode72HookAddr);
      g_mode72HookAddr = nullptr;
      g_origMode72SetVS = nullptr;
    }
    g_mode72Armed.store(false);
    g_mode74GateOpen.store(false);
    WriteStereoModeFile(45);
    Log("Mode74: wrote stereo=45 after SetVSConstF fault");
    return;
  }

  Mode74UpdateStreamingGate();

  // Dual frames that ran BuildRootA×2 skip temporal capture (Mode 75 only).
  // HOLD last true L/R only while gate OPEN (sparse dual active).
  // Mode77/80/87/88/89 DrawScene dual: MUST HOLD — TemporalCapture would overwrite
  // same-frame L/R (bug fixed 2026-07-26). Mode88 off-ticks also HOLD via flag.
  // Mode 74 hitchcut → pair-hold. Mode 76 → AER. Dual CLOSED → same.
  const bool dualCaptured = g_mode74DidDualThisFrame.exchange(false);
  const bool gateOpen = IsSparseSessionDual(GetStereoMode()) && g_mode74GateOpen.load() &&
                        !g_mode74DualDead.load() && !g_mode74DualSessionOff.load() &&
                        g_mode74DualEnabled.load();
  const bool haveTruePair = g_haveL && g_haveR && g_mode74DualN.load() > 0;
  const bool holdSparsePair = dualCaptured || (gateOpen && haveTruePair);
  const bool holdDrawScenePair =
      IsDrawSceneOnlyDual(GetStereoMode()) && g_haveL && g_haveR &&
      (dualCaptured || g_mode77DualN.load() > 0);
  if (holdSparsePair || holdDrawScenePair) {
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
      D3DSURFACE_DESC desc{};
      bb->GetDesc(&desc);
      bb->Release();
      uint32_t rtW = desc.Width, rtH = desc.Height;
      ComputeCanvasSize(desc.Width, desc.Height, &rtW, &rtH);
      EnsureEyeRts(device, rtW, rtH);
    }
    g_mode72Budget.store(1);
    float v16[16]{};
    if (BuildLiveViewMatrix16(v16)) {
      __try {
        device->SetVertexShaderConstantF(0, v16, 4);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
  } else if (IsAerPresenceMode(GetStereoMode())) {
    // Mode 76 AER done right (Approach D): one engine eye/frame + TextureWithPose.
    // Pose/seated frozen in Mode74SyncFrameDesc; eye matrices via DRAW inject for
    // active eye only. Jump risk remains — headset decides tomorrow.
    TemporalCaptureAerMode74(device);
    g_mode72Budget.store(1);
  } else {
    // Mode 74 hitchcut + Mode 75 dual-closed + Mode 78/79: pair-hold temporal.
    TemporalCapturePairHold(device);
    g_mode72Budget.store(1);
  }
  // VIEW budget: Mode78 higher (Approach C). Dual OPEN keeps 12. Else hitchcut=1.
  if (gateOpen)
    g_mode74DualViewBudget.store(kMode74DualViewBudgetPerEye);
  else if (IsShaderConstStereo(GetStereoMode()))
    g_mode74DualViewBudget.store(kMode78ViewBudget);
  else
    g_mode74DualViewBudget.store(kMode74TemporalViewBudget);

  static uint32_t s_es = 0;
  static DWORD s_fpsTick = 0;
  static uint32_t s_fpsEs0 = 0;
  ++s_es;
  if (s_fpsTick == 0) {
    s_fpsTick = nowEs;
    s_fpsEs0 = s_es;
  }
  // Quieter when dual OFF (street path); still log hitchy lastEs (dedupe streaks).
  const bool dualOffQuiet = !g_mode74DualOptIn.load() || g_mode74DualSessionOff.load() ||
                            IsHitchCutEyeProj(GetStereoMode());
  const uint32_t logEvery = dualOffQuiet ? 1200u : 120u;
  const DWORD lastEs = g_mode74LastEsMs.load();
  static DWORD s_lastHitchLogEs = 0;
  const bool hitchLog =
      lastEs >= kMode74HitchMs && (s_es - s_lastHitchLogEs) >= 30u;
  if (hitchLog)
    s_lastHitchLogEs = s_es;
  if (s_es <= 6 || (s_es % logEvery) == 0 || hitchLog)
    Log("Mode74: es#%u gate=%s dualN=%u liveStreak=%u miss=%u sessOff=%d injects=%u "
        "contentInj=%u cacheInj=%u uploadFnCalls=%u uploadEye=%u uploadDead=%d "
        "pubProj=%u c0=%u eyeProj=%u hookCalls=%u sep=%.0fcm every=1/%u dualEn=%d "
        "optIn=%d lastEs=%ums dualMs=%u budgSkip=%u hold=%d viewBudg=%u "
        "(mode=%d hitchcut/dual/AER; kill=45)",
        s_es, g_mode74GateOpen.load() ? "OPEN" : "CLOSED", g_mode74DualN.load(),
        g_mode74LiveStreak.load(), g_mode74MissStreak.load(),
        g_mode74DualSessionOff.load() ? 1 : 0, g_mode72Injects.load(),
        g_mode74ContentInjects.load(), g_mode74CachedViewInjects.load(),
        g_mode74UploadFnCalls.load(), g_mode74UploadFnEyeWrites.load(),
        g_mode74UploadFnDead.load() ? 1 : 0, g_mode74PublishProjInjects.load(),
        g_mode74PubC0Writes.load(), g_mode74ProjInjects.load(),
        g_mode74PublishProjCalls.load(), GetStereoSepMeters() * 100.f, kMode74DualEveryN,
        g_mode74DualEnabled.load() ? 1 : 0, g_mode74DualOptIn.load() ? 1 : 0, lastEs,
        g_mode74LastDualMs.load(), g_mode74DualSkipRemain.load(),
        (holdSparsePair || holdDrawScenePair) ? 1 : 0,
        gateOpen ? kMode74DualViewBudgetPerEye : kMode74TemporalViewBudget,
        static_cast<int>(GetStereoMode()));
  // App FPS estimate: quieter when dual OFF / Mode88 (every 2s).
  const DWORD fpsEveryMs = dualOffQuiet ? 2000u : 1000u;
  // Hitch histogram (street probe) — count every ES, dump every 5s with AppFPS.
  static uint32_t s_h0_16 = 0, s_h17_33 = 0, s_h34_50 = 0, s_h51p = 0;
  static DWORD s_histTick = 0;
  if (s_histTick == 0)
    s_histTick = nowEs;
  {
    const DWORD le = g_mode74LastEsMs.load();
    if (le <= 16)
      ++s_h0_16;
    else if (le <= 33)
      ++s_h17_33;
    else if (le <= 50)
      ++s_h34_50;
    else
      ++s_h51p;
  }
  if (nowEs - s_fpsTick >= fpsEveryMs) {
    const uint32_t dEs = s_es - s_fpsEs0;
    const DWORD dMs = nowEs - s_fpsTick;
    const float esPerSec = dMs > 0 ? (1000.f * static_cast<float>(dEs) / static_cast<float>(dMs))
                                   : 0.f;
    Log("Mode74: AppFPS es/s=%.1f lastEs=%ums dualMs=%u dualN=%u hold=%d gate=%s "
        "drawDualN=%u (want es/s ≈ submit/s; hold=1 means L/R reused)",
        esPerSec, g_mode74LastEsMs.load(), g_mode74LastDualMs.load(), g_mode74DualN.load(),
        (holdSparsePair || holdDrawScenePair) ? 1 : 0, gateOpen ? "OPEN" : "CLOSED",
        g_mode77DualN.load());
    if (nowEs - s_histTick >= 5000u) {
      Log("HitchHist: lastEs buckets 0-16=%u 17-33=%u 34-50=%u 51+=%u (5s; street probe)",
          s_h0_16, s_h17_33, s_h34_50, s_h51p);
      s_h0_16 = s_h17_33 = s_h34_50 = s_h51p = 0;
      s_histTick = nowEs;
    }
    s_fpsTick = nowEs;
    s_fpsEs0 = s_es;
  }
  if (g_mode74UploadFnArmed.load() && g_mode74UploadFnCalls.load() == 0 && s_es == 180) {
    Log("UploadFn: SILENT after %u ES (0x2A1E10 not hit) — CE hot path is ReplayDispatch "
        "→ SetVSConstF 0x220D0; Mode72 contentinj+toeIn owns sep; keep UploadFn armed",
        s_es);
  }
  // StereoDiff: prove L/R canvases differ (geometry parallax vs post noise).
  if (g_haveL && g_haveR && (s_es == 90 || (s_es % 300) == 0)) {
    const long long diff = CompareEyeCanvases(device);
    if (diff >= 0)
      Log("Mode74: StereoDiff absdiff=%lld (>>0 = L≠R reached pixels; 0 = still flat/"
          "identical)",
          diff);
  }
}

bool Mode65ReadDeviceVtable(uint32_t* outDevice, uint32_t* outVt, uint32_t* out178,
                            uint32_t* out1B4, uint32_t* outGate, uint32_t* outActiveView) {
  __try {
    const auto* devPtr = reinterpret_cast<const uint32_t*>(kMode65DeviceGlobalVa);
    const auto* gatePtr = reinterpret_cast<const uint32_t*>(kMode65ReplayGateVa);
    const auto* activePtr = reinterpret_cast<const uint32_t*>(kMode65ActiveViewVa);
    const uint32_t device = *devPtr;
    const uint32_t gate = *gatePtr;
    const uint32_t activeView = *activePtr;
    if (outGate)
      *outGate = gate;
    if (outActiveView)
      *outActiveView = activeView;
    if (!device) {
      if (outDevice)
        *outDevice = 0;
      if (outVt)
        *outVt = 0;
      if (out178)
        *out178 = 0;
      if (out1B4)
        *out1B4 = 0;
      return true;
    }
    const auto* vtPtr = reinterpret_cast<const uint32_t*>(device);
    const uint32_t vt = *vtPtr;
    const uint32_t slot178 =
        vt ? *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(vt) + 0x178) : 0;
    const uint32_t slot1B4 =
        vt ? *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(vt) + 0x1B4) : 0;
    if (outDevice)
      *outDevice = device;
    if (outVt)
      *outVt = vt;
    if (out178)
      *out178 = slot178;
    if (out1B4)
      *out1B4 = slot1B4;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Mode65OnEndScene(IDirect3DDevice9* device) {
  TemporalCapturePairHold(device);
  if (g_mode65LogDone.load())
    return;

  const uint32_t es = ++g_mode65Es;
  uint32_t dev = 0, vt = 0, s178 = 0, s1B4 = 0, gate = 0, active = 0;
  if (!Mode65ReadDeviceVtable(&dev, &vt, &s178, &s1B4, &gate, &active)) {
    Log("Mode65: READ FAIL @ EndScene es#%u — wrote stereo=45", es);
    g_mode65LogDone.store(true);
    WriteStereoModeFile(45);
    return;
  }

  const bool changed =
      dev != g_mode65LastDevice || s178 != g_mode65LastVt178 || s1B4 != g_mode65LastVt1B4;
  if (es <= 4 || changed || (es % 15) == 0) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const uint32_t devRva = dev ? static_cast<uint32_t>(dev - base) : 0;
    const uint32_t vtRva = vt ? static_cast<uint32_t>(vt - base) : 0;
    const uint32_t r178 = s178 ? static_cast<uint32_t>(s178 - base) : 0;
    const uint32_t r1B4 = s1B4 ? static_cast<uint32_t>(s1B4 - base) : 0;
    const uint32_t actRva = active ? static_cast<uint32_t>(active - base) : 0;
    Log("Mode65: VT178-LOG es#%u/%u device=0x%X rva=0x%X vtable=0x%X slot178=0x%X "
        "slot1B4=0x%X replayGate=%u activeView=0x%X hook=NO",
        es, kMode65LogEsNeed, dev, devRva, vtRva, r178, r1B4, gate, actRva);
    // First bytes of slot178 target (compare with live ReplayDispatch path @0x30D0D).
    if (s178) {
      __try {
        const auto* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(s178));
        Log("Mode65: slot178 bytes %02X %02X %02X %02X %02X %02X %02X %02X "
            "(want stable target; SameFrameSeamGate=CLOSED)",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Mode65: slot178 bytes UNREADABLE");
      }
    }
  }
  g_mode65LastDevice = dev;
  g_mode65LastVt178 = s178;
  g_mode65LastVt1B4 = s1B4;

  if (es < kMode65LogEsNeed)
    return;

  g_mode65LogDone.store(true);
  Log("Mode65: VT178-LOG done (%u EndScenes) — compare slot178 with Mode42 OWNER-EDGE "
      "rets 0x30D13 / 0x2A2183 / 0x2A25FF (ReplayGate triad 0x30D0D+0x2A217D+0x2A25F9); "
      "next: stereo=66 then 67; SameFrameSeamGate=CLOSED hook=NO",
      kMode65LogEsNeed);
  WriteStereoModeFile(45);
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
      mode == StereoMode::FovCanvasMotionGuardRtLock || IsHeadOwnedCamFamily(mode)) {
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

  // Mode 38 AER + Mode74 sparse HOLD: stamp capture-time HMD pose so SteamVR can
  // reproject stale eyes between duals (OpenVR #1253). Fall back to Submit_Default.
  vr::EVRCompositorError err = vr::VRCompositorError_None;
  const StereoMode sm = GetStereoMode();
  const bool wantPose = sm == StereoMode::AerPoseSubmit || IsMode74Family(sm);
  const bool poseOk =
      wantPose && ((eye == vr::Eye_Left) ? g_submitPoseLValid : g_submitPoseRValid);
  if (poseOk) {
    vr::VRTextureWithPose_t tp{};
    tp.handle = &vd;
    tp.eType = vr::TextureType_Vulkan;
    tp.eColorSpace = vr::ColorSpace_Gamma;
    tp.mDeviceToAbsoluteTracking =
        (eye == vr::Eye_Left) ? g_submitPoseL : g_submitPoseR;
    err = vr::VRCompositor()->Submit(eye, &tp, nullptr, vr::Submit_TextureWithPose);
    static uint32_t s_aer = 0;
    if ((++s_aer) <= 8 || (s_aer % 300) == 0)
      Log("AERPose: eye=%s submitWithPose=1 err=%d mode=%d",
          eye == vr::Eye_Left ? "L" : "R", static_cast<int>(err), static_cast<int>(sm));
    if (err != vr::VRCompositorError_None) {
      // Runtime may ignore pose (historic WMR bug) — still try default once.
      err = vr::VRCompositor()->Submit(eye, &t, nullptr, vr::Submit_Default);
      if (s_aer <= 8 || (s_aer % 300) == 0)
        Log("AERPose: eye=%s pose-submit failed → Default err=%d",
            eye == vr::Eye_Left ? "L" : "R", static_cast<int>(err));
    }
  } else {
    if (wantPose) {
      static uint32_t s_miss = 0;
      if ((++s_miss) <= 4 || (s_miss % 300) == 0)
        Log("AERPose: eye=%s pose missing → Submit_Default",
            eye == vr::Eye_Left ? "L" : "R");
    }
    // Fusion baseline: nullptr bounds + temporal IPD only.
    err = vr::VRCompositor()->Submit(eye, &t, nullptr, vr::Submit_Default);
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

void __fastcall HookBuildRenderList(void* self, void* edx) {
  if (!g_origBuild)
    return;

  g_lastThisDraw = self;
  const StereoMode mode = GetStereoMode();
  if (mode == StereoMode::RenderRootProbe)
    LogRenderRootRet("BuildDrawScene", _ReturnAddress());

  // Mode 77: cheaper same-frame dual — DrawScene×2 only (Approach A).
  if (IsDrawSceneOnlyDual(mode) && !g_mode77DualDead.load() && !g_inDual.load() &&
      IsCamMatrixOverrideEnabled() && g_device && g_texL && g_texR && g_mode72Armed.load()) {
    static DWORD s_prev77 = 0;
    const DWORD now77 = GetTickCount();
    const DWORD gap77 = (s_prev77 != 0) ? (now77 - s_prev77) : 0;
    s_prev77 = now77;
    // Soft hitch skip — keep mono DrawScene this frame.
    if (gap77 >= kMode77HitchMs) {
      g_origBuild(self, edx);
      return;
    }
    static uint32_t s_77Gate = 0;
    if (s_77Gate < kMode77GateNeedLiveEs) {
      ++s_77Gate;
      g_origBuild(self, edx);
      return;
    }
    // Mode86/88: dual every N DrawScene — cuts StretchRect×2 + DrawScene×2 hitch.
    // Off ticks: mono DrawScene + HOLD last true L/R (do NOT TemporalCapture overwrite).
    // Mode88 keeps EyeProj ON; Mode86 does not (A/B only — jumping).
    // Mode88: gtaiv_dxvk_vr.dualn (2=default, 3=lighter). Mode86 fixed 2.
    if (WantsDualEveryOther(mode)) {
      static uint32_t s_dualTick = 0;
      ++s_dualTick;
      const uint32_t everyN =
          IsHitchCutEyeProj(mode) ? GetDualEveryN() : 2u;
      if ((s_dualTick % everyN) != 0u) {
        g_origBuild(self, edx);
        if (g_haveL && g_haveR)
          g_mode74DidDualThisFrame.store(true);  // HOLD last pair
        return;
      }
    }
    g_inDual.store(true);
    const bool ok = RunMode77DrawSceneDualGuarded(self, edx);
    g_inDual.store(false);
    if (!ok) {
      g_mode77DualDead.store(true);
      g_haveL = g_haveR = false;
      Log("Mode77: EXCEPTION in DrawScene-only dual — permanently disabled; wrote stereo=74");
      WriteStereoModeFile(74);
      g_origBuild(self, edx);
      return;
    }
    g_mode74DidDualThisFrame.store(true);  // EndScene HOLD — protect same-frame L/R
    const uint32_t n = ++g_mode77DualN;
    if (n <= 6 || (n % 120) == 0)
      Log("Mode77: DRAWSCENE-ONLY dual #%u haveL=%d haveR=%d injects=%u sep=%.0fcm "
          "(Approach A; not BuildRootA×2; kill=45)",
          n, g_haveL ? 1 : 0, g_haveR ? 1 : 0, g_mode72Injects.load(),
          GetStereoSepMeters() * 100.f);
    if (g_haveL && g_haveR && (n == 5 || (n % 300) == 0))
      CompareEyeCanvases(g_device);
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
      if (Mode40NeedsMonoGuard(&rotDeg, &moveCm)) {
        // Both eye canvases now originate from this exact backbuffer epoch.
        // They retain their correct per-eye canvas geometry, but intentionally
        // contain one camera view until the next calm L/R temporal pair.
        const bool fast = GetStereoMode() == StereoMode::FovCanvasMotionGuardFast ||
                          GetStereoMode() == StereoMode::FovCanvasMotionGuardRtLock ||
                          IsHeadOwnedCamFamily(GetStereoMode());
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

// Mode74 AER (RealVR pattern): one engine eye per EndScene into THAT eye's submit RT.
// Every Present still Submits L+R with Submit_TextureWithPose:
//   - fresh eye: this frame texture + capture-time (WaitGetPoses) pose
//   - held eye: last texture + its capture-time pose (SteamVR ASW/timewarp)
// NOT pair-hold (no 2-frame promote lag). NOT BuildRootA×2.
void TemporalCaptureAerMode74(IDirect3DDevice9* device) {
  if (!device || !g_texL || !g_texR)
    return;
  LogCachedIpdOnce();
  // FrameDesc already synced in Mode74OnEndScene — do not re-sample pose mid-ES.
  const StereoEye eye = GetStereoEye();
  const bool right = (eye == StereoEye::Right);
  IDirect3DTexture9* dst = right ? g_texR : g_texL;
  const vr::EVREye vrEye = right ? vr::Eye_Right : vr::Eye_Left;
  if (CopyBbToEyeCanvas(device, dst, vrEye)) {
    SnapshotHoldPose(right);
    if (right) {
      g_submitPoseR = g_holdPoseR;
      g_submitPoseRValid = g_holdPoseRValid;
      g_haveR = true;
    } else {
      g_submitPoseL = g_holdPoseL;
      g_submitPoseLValid = g_holdPoseLValid;
      g_haveL = true;
    }
  }
  // Next engine frame renders the other eye (RVRGetFrameDesc bit flip).
  SetStereoEye(right ? StereoEye::Left : StereoEye::Right);
  RefreshLiveCamForStereoEye();
  const uint32_t n = ++g_temporalFrames;
  if (n <= 6 || (n % 120) == 0)
    Log("Mode76AER: frame #%u captured=%s haveL=%d haveR=%d poseLR=%d/%d sep=%.0fcm "
        "cover=(%.3f,%.3f) (Submit both TextureWithPose; dual×2 off)",
        n, right ? "R" : "L", g_haveL ? 1 : 0, g_haveR ? 1 : 0,
        g_submitPoseLValid ? 1 : 0, g_submitPoseRValid ? 1 : 0,
        GetStereoSepMeters() * 100.f, g_mode74FrameDesc.coverTanH,
        g_mode74FrameDesc.coverTanV);
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

// Graphics-options / resolution change: D3DPOOL_DEFAULT eye RTs die on Reset.
// Must release BEFORE device Reset (dangling pointers → hang/freeze).
void StereoNotifyDeviceLost() {
  const bool had = g_texL || g_texR || g_holdL || g_holdR || g_resolveSurf;
  ReleaseEyeRts();
  g_device = nullptr;
  if (had)
    Log("DeviceReset: released eye RTs (POOL_DEFAULT) before/after Lost — will recreate");
}

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

  if (mode == StereoMode::PhaseDualDeviceVs || mode == StereoMode::FovRecomputeSite ||
      mode == StereoMode::FovRecomputeTrueCanvas || mode == StereoMode::FovCanvasComfort ||
      mode == StereoMode::AerPoseSubmit || mode == StereoMode::FovCanvasLowMotion ||
      mode == StereoMode::FovCanvasMotionGuard || mode == StereoMode::ReplayCallChainProbe ||
      mode == StereoMode::ReplayOwnerCountProbe ||
      mode == StereoMode::FovCanvasMotionGuardFast ||
      mode == StereoMode::FovCanvasMotionGuardRtLock || IsHeadOwnedCamFamily(mode)) {
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
    } else if (mode == StereoMode::FovCanvasMotionGuardRtLock || IsHeadOwnedCamFamily(mode)) {
      const bool fovOk = InstallFovRecomputeSiteHook();
      g_mode40MonoPairs.store(0);
      g_mode44RtChecks.store(0);
      g_mode44RtRecreates.store(0);
      g_mode44RtSuppressed.store(0);
      if (mode == StereoMode::RePatternValidate) {
        LogRePatternValidationOnce();
        Log("StereoRender: mode 62 RE-PATTERN VALIDATE (Mode45 renderer + offline AOB "
            "check; docs/RE_OFFSETS.md; SameFrame=0) ok=%d fovSite=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
        Log("StereoRender: kill-switch - stereo=45 or 30 + delete gtaiv_dxvk_vr.fovadd");
      } else if (mode == StereoMode::SameFrameSeamGate) {
        LogRePatternValidationOnce();
        Log("StereoRender: mode 63 SAME-FRAME SEAM GATE (Mode45 renderer; "
            "SameFrameSeamGateOpen=%d — dual stays OFF until RE proves replay owner) "
            "ok=%d fovSite=%d",
            IsSameFrameSeamGateOpen() ? 1 : 0, g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
        Log("StereoRender: kill-switch - stereo=45; see docs/RE_OFFSETS.md forbidden list");
      } else if (mode == StereoMode::ViewConstCountProbe) {
        LogRePatternValidationOnce();
        g_mode64Es.store(0);
        g_mode64CountEs.store(0);
        g_mode64EntrySum = 0;
        g_mode64CountDone.store(false);
        const bool countOk = Mode64InstallCountHook();
        Log("StereoRender: mode 64 VIEW-CONST COUNT (Mode45 renderer; AOB→mapped TRUE start "
            "0x32470; COUNT-only ≥45 ES; dual=OFF; default stereo stays 45) ok=%d fovSite=%d "
            "countHook=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0, countOk ? 1 : 0);
        Log("StereoRender: how-to — edit gtaiv_dxvk_vr.stereo to 64; play 1 calm minute; "
            "read Mode64: COUNT lines; file reverts to 45 when done");
        Log("StereoRender: kill-switch - stereo=45 or delete gtaiv_dxvk_vr.stereo");
      } else if (mode == StereoMode::ReplayVtable178Log) {
        LogRePatternValidationOnce();
        g_mode65Es.store(0);
        g_mode65LogDone.store(false);
        g_mode65LastDevice = g_mode65LastVt178 = g_mode65LastVt1B4 = 0;
        Log("StereoRender: mode 65 VT178-LOG (Mode45 renderer; read-only EndScene peek "
            "at [0x17ed8d8] vtable +0x178/+0x1B4; NO game hook) ok=%d fovSite=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
        Log("StereoRender: how-to — edit gtaiv_dxvk_vr.stereo to 65; play ≥45 EndScenes; "
            "read Mode65: VT178-LOG lines; file reverts to 45 when done");
        Log("StereoRender: kill-switch - stereo=45 or delete gtaiv_dxvk_vr.stereo");
      } else if (mode == StereoMode::PublishSyncCountProbe) {
        LogRePatternValidationOnce();
        g_mode66Es.store(0);
        g_mode66CountEs.store(0);
        g_mode66EntrySum = 0;
        g_mode66CountDone.store(false);
        const bool countOk = Mode66InstallCountHook();
        Log("StereoRender: mode 66 PUBLISHSYNC COUNT (Mode45 renderer; AOB→mapped 0x30F00; "
            "COUNT-only ≥45 ES; dual=OFF; no 0x30CD0 hook) ok=%d fovSite=%d countHook=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0, countOk ? 1 : 0);
        Log("StereoRender: how-to — edit gtaiv_dxvk_vr.stereo to 66; play 1 calm minute; "
            "read Mode66: COUNT lines; file reverts to 45 when done");
        Log("StereoRender: kill-switch - stereo=45 or delete gtaiv_dxvk_vr.stereo");
      } else if (mode == StereoMode::ViewMatWriterCountProbe) {
        LogRePatternValidationOnce();
        g_mode67Es.store(0);
        g_mode67CountEs.store(0);
        g_mode67EntrySum = 0;
        g_mode67CountDone.store(false);
        const bool countOk = Mode67InstallCountHook();
        Log("StereoRender: mode 67 VIEWMAT COUNT (Mode45 renderer; AOB→mapped 0x314C0; "
            "COUNT-only ≥45 ES; dual=OFF) ok=%d fovSite=%d countHook=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0, countOk ? 1 : 0);
        Log("StereoRender: how-to — edit gtaiv_dxvk_vr.stereo to 67; play 1 calm minute; "
            "read Mode67: COUNT lines; file reverts to 45 when done");
        Log("StereoRender: kill-switch - stereo=45 or delete gtaiv_dxvk_vr.stereo");
      } else if (mode == StereoMode::ReplayDispatchCountProbe) {
        LogRePatternValidationOnce();
        g_mode68Es.store(0);
        g_mode68CountEs.store(0);
        g_mode68EntrySum = 0;
        g_mode68CountDone.store(false);
        const bool countOk = Mode68InstallCountHook();
        Log("StereoRender: mode 68 REPLAYDISPATCH COUNT (Mode45 renderer; AOB→mapped 0x30CD0; "
            "COUNT-only ≥45 ES; dual=OFF; bridge→slot178 0x220D0) ok=%d fovSite=%d countHook=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0, countOk ? 1 : 0);
        Log("StereoRender: how-to — edit gtaiv_dxvk_vr.stereo to 68; play 1 calm minute; "
            "read Mode68: COUNT lines; file reverts to 45 when done");
        Log("StereoRender: kill-switch - stereo=45 or delete gtaiv_dxvk_vr.stereo");
      } else if (mode == StereoMode::ReplayDispatchMatPeek) {
        LogRePatternValidationOnce();
        g_mode69Es.store(0);
        g_mode69PeekEs.store(0);
        g_mode69WaitEs.store(0);
        g_mode69Live.store(false);
        g_mode69PeekDone.store(false);
        const bool peekOk = Mode69InstallPeekHook();
        Log("StereoRender: mode 69 REPLAYDISPATCH MAT-PEEK (Mode45 renderer; AOB→mapped 0x30CD0; "
            "READ-ONLY view+0x80; WAIT for live then sample; dual=OFF) ok=%d fovSite=%d peekHook=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0, peekOk ? 1 : 0);
        Log("StereoRender: how-to — edit gtaiv_dxvk_vr.stereo to 69; play 1 calm minute; "
            "read Mode69: PEEK lines; file reverts to 45 when done");
        Log("StereoRender: kill-switch - stereo=45 or delete gtaiv_dxvk_vr.stereo");
      } else if (mode == StereoMode::ReplayDispatchActivePeek) {
        LogRePatternValidationOnce();
        g_mode70WaitEs.store(0);
        g_mode70PeekEs.store(0);
        g_mode70Live.store(false);
        g_mode70PeekDone.store(false);
        const bool peekOk = Mode70InstallPeekHook();
        Log("StereoRender: mode 70 REPLAYDISPATCH ACTIVE-PEEK (Mode45 renderer; filter "
            "self==[0x17F583C]+live; READ-ONLY; dual=OFF) ok=%d fovSite=%d peekHook=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0, peekOk ? 1 : 0);
        Log("StereoRender: how-to — stereo=70; load into world; read Mode70: PEEK lines");
        Log("StereoRender: kill-switch - stereo=45 or delete gtaiv_dxvk_vr.stereo");
      } else if (mode == StereoMode::ReplayDispatchTemporalInject) {
        LogRePatternValidationOnce();
        g_mode71Armed.store(false);
        g_mode71Dead.store(false);
        const bool okHook = Mode71InstallInjectHook();
        Log("StereoRender: mode 71 REPLAYDISPATCH TEMPORAL-INJECT (Mode45 capture; "
            "±IPD/2 at activeView+live view+0x80; restore; SameFrameSeamGate=CLOSED) "
            "ok=%d fovSite=%d injectHook=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0, okHook ? 1 : 0);
        Log("StereoRender: how-to — stereo=71; load world; look for L≠R parallax; kill=45");
        Log("StereoRender: kill-switch - stereo=45 or delete gtaiv_dxvk_vr.stereo");
      } else if (mode == StereoMode::VsConstTemporalInject) {
        LogRePatternValidationOnce();
        g_mode72Armed.store(false);
        g_mode72Dead.store(false);
        const bool okHook = Mode72InstallHook();
        Log("StereoRender: mode 72 VSCONST TEMPORAL-INJECT (Mode45 capture; SetVSConstF "
            "0x220D0 src-COPY ±IPD/2; never writes view object; SameFrameSeamGate=CLOSED) "
            "ok=%d fovSite=%d injectHook=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0, okHook ? 1 : 0);
        Log("StereoRender: how-to — stereo=72; load world; check stability + parallax; kill=45");
        Log("StereoRender: kill-switch - stereo=45 or delete gtaiv_dxvk_vr.stereo");
      } else if (mode == StereoMode::SameFrameVsConstDual) {
        LogRePatternValidationOnce();
        g_mode72Armed.store(false);
        g_mode72Dead.store(false);
        g_mode73DualDead.store(false);
        g_mode73DualN.store(0);
        g_haveL = g_haveR = false;
        const bool okHook = Mode72InstallHook();
        Log("StereoRender: mode 73 SAME-FRAME VSCONST DUAL (BuildRootA×2 L/R + SetVSConstF "
            "src-COPY; never writes view+0x80; motion-guard OFF) ok=%d fovSite=%d vsHook=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0, okHook ? 1 : 0);
        Log("StereoRender: how-to — stereo=73; load world; expect Mode73: SAME-FRAME dual; "
            "kill=45 on exception");
        Log("StereoRender: kill-switch - stereo=72 or 45");
      } else if (IsMode74Family(mode)) {
        LogRePatternValidationOnce();
        g_mode72Armed.store(false);
        g_mode72Dead.store(false);
        g_mode74GateOpen.store(false);
        g_mode74DualDead.store(false);
        g_mode74DualSessionOff.store(false);
        // Mode 75 = dual path (always opt-in). Mode 74/76/78/79 never dual.
        const bool dualOptIn = IsSparseSessionDual(mode);
        g_mode74DualOptIn.store(dualOptIn);
        g_mode74DualEnabled.store(false);
        g_mode74DualN.store(0);
        g_mode74LiveStreak.store(0);
        g_mode74MissStreak.store(0);
        g_mode74WorldWeakStreak.store(0);
        g_mode74SoftSkipStreak.store(0);
        g_mode74DualSkipRemain.store(0);
        g_mode74LastDualMs.store(0);
        g_mode74LastEsMs.store(0);
        g_mode74BuildTick.store(0);
        g_mode74DidDualThisFrame.store(false);
        g_mode74LastInjects = 0;
        g_mode74DualViewBudget.store(IsShaderConstStereo(mode) ? kMode78ViewBudget
                                                               : kMode74TemporalViewBudget);
        g_mode74ContentInjects.store(0);
        g_mode74CachedViewInjects.store(0);
        g_mode77DualN.store(0);
        g_mode77DualDead.store(false);
        g_mode79FovProofN.store(0);
        g_haveL = g_haveR = false;
        const bool okHook = Mode72InstallHook();
        const bool okPub = Mode74InstallPublishProjHook();
        const bool okUpload = Mode74InstallUploadFnHook();
        bool okDraw = true;
        if (IsDrawSceneOnlyDual(mode)) {
          // Approach A: also hook DrawScene BuildRenderList for cheaper dual.
          okDraw = HookOneBuild("DrawScene", FindDrawSceneBuildRenderList(),
                                reinterpret_cast<void*>(&HookBuildRenderList), &g_origBuild);
        }
        if (mode == StereoMode::SameFrameVsConstGatedDual) {
          Log("Mode74: HITCHCUT BASELINE (default) — pair-hold temporal + DRAW HMD look/"
              "IPD; dual×2 OFF; viewBudg=1; morning-safe; kill=45");
        } else if (mode == StereoMode::SparseSessionDual) {
          Log("Mode75: SAFER SPARSE DUAL — gated BuildRootA×2 1/%u after %u street-stable "
              "live ES; denser fearvr 1/2 SCRAPPED (felt LESS 3D); hitch≥%ums → SESSION OFF; "
              "kill=45",
              kMode75DualEveryN, kMode75GateNeedLiveEs, kMode75HitchMs);
        } else if (mode == StereoMode::AerPresenceExperimental) {
          Log("Mode76: AER QUALITY — one engine eye/frame + TextureWithPose + eye matrices; "
              "dual×2 OFF; jump risk; kill=45");
        } else if (mode == StereoMode::DrawSceneOnlyDual) {
          Log("Mode77: DRAWSCENE-ONLY DUAL (Approach A) — DrawScene×2 not BuildRootA×2; "
              "gate=%u ES hitch≥%ums; okDraw=%d; FOV canvas GATED until drawn sy<1.25; "
              "kill=45",
              kMode77GateNeedLiveEs, kMode77HitchMs, okDraw ? 1 : 0);
        } else if (mode == StereoMode::ShaderConstStereo) {
          Log("Mode78: SHADER-CONST STEREO (Approach C) — high-rate VIEW/PROJ inject "
              "budget=%u/ES; no BuildRootA×2; pair-hold capture; kill=45",
              kMode78ViewBudget);
        } else if (mode == StereoMode::FovPresenceHammer) {
          Log("Mode79: FOV PRESENCE HAMMER — CCam hard + drawn-FOV proof + honest canvas "
              "(no 162%%h lie); EyeProj on; kill=45");
        } else if (mode == StereoMode::FovPresenceDrawScene) {
          Log("Mode80: ASPECT-SAFE LETTERBOX — Mode77 dual + FOV prove + "
              "gameTan aspect=BB letterbox (fill~100%%h/62%%v on G2); okDraw=%d; kill=45",
              okDraw ? 1 : 0);
        } else if (mode == StereoMode::AspectLetterboxSafe) {
          Log("Mode81: LETTERBOX-SAFE — Mode77 dual + softer cover-H FOV; bars OK; "
              "never squash; okDraw=%d; kill=45",
              okDraw ? 1 : 0);
        } else if (mode == StereoMode::AspectFillAggressive) {
          Log("Mode82: FILL-HMD AGGRESSIVE — Mode77 dual + cover-V FOV; canvas from "
              "MEASURED drawn FOV only (no giant lie); may crop sides; okDraw=%d; kill=45",
              okDraw ? 1 : 0);
        } else if (mode == StereoMode::AspectFillSweet) {
          Log("Mode83: FILL-SWEET — Mode77 dual + ~90%%v fill (headset: 149%%h monitor; "
              "prefer 84) + quiet EyeProj; okDraw=%d; kill=45",
              okDraw ? 1 : 0);
        } else if (mode == StereoMode::LetterboxWideFov) {
          Log("Mode84: SOFT-LETTERBOX (A/B) — ~78%%v / fillH≤118%%; headset groß — prefer 85; "
              "okDraw=%d; kill=45",
              okDraw ? 1 : 0);
        } else if (mode == StereoMode::SteamVrRtLetterbox) {
          Log("Mode85: STEAMVR-RT + MODE80 LETTERBOX — eye RTs from "
              "GetRecommendedRenderTargetSize (cap 2048); hard letterbox; Mode77 dual + "
              "quiet EyeProj; A/B only — prefer 87; okDraw=%d; kill=45",
              okDraw ? 1 : 0);
        } else if (mode == StereoMode::SteamVrRtSafe) {
          Log("Mode86: STEAMVR-RT NO-EYEPROJ (A/B only) — mid-draw EyeProj OFF; eye RT "
              "min(rec,BB*1.25,1440); Mode80 letterbox; DrawScene dual 1/2; headset jumping "
              "worse — prefer 87; okDraw=%d; kill=45",
              okDraw ? 1 : 0);
        } else if (mode == StereoMode::ConfigEyeRtLetterbox) {
          (void)GetConfiguredEyeRtDim();  // log eyert tier at arm
          Log("Mode87: CONFIG-EYERT + EyeProj (quality A/B) — EyeProj ON mid-draw; fixed "
              "square eyeRT from gtaiv_dxvk_vr.eyert (default 2048); NOT SteamVR recommended; "
              "Mode77 dual every frame; Mode80 letterbox; CCam/PubProj cull sync; "
              "okDraw=%d; prefer 88 for streets; kill=45",
              okDraw ? 1 : 0);
        } else if (mode == StereoMode::HitchCutEyeProj) {
          (void)GetConfiguredEyeRtDim();
          Log("Mode88: HITCHCUT + EyeProj DEFAULT — EyeProj ON; dual every %u DrawScene "
              "+ HOLD last L/R (gtaiv_dxvk_vr.dualn 2..4); eyert default 1440; quiet "
              "EyeProj after FOV proven; Mode80 letterbox; Reset-safe eye RTs; never "
              "Mode86; okDraw=%d; kill=45",
              GetDualEveryN(), okDraw ? 1 : 0);
        } else if (mode == StereoMode::CullSyncPresence) {
          (void)GetConfiguredEyeRtDim();
          Log("Mode89: CULL-SYNC PRESENCE (A/B) — EyeProj ON; dual every frame; quieter "
              "logs; aggressive CCam/PubProj cull sync; mild soft letterbox ~68%%v/"
              "fillH≤108%% (never Mode83; not Mode84 78%% groß); Reset-safe RTs; "
              "okDraw=%d; kill=45",
              okDraw ? 1 : 0);
        } else {
          Log("Mode74family: unknown mode %d", static_cast<int>(mode));
        }
        Log("StereoRender: mode %d family (74=hitchcut 75=saferDual 76=AER 77=drawScene "
            "78=shader 79=fov 80=letterbox 81=softLB 82=fill 83=fillSweet 84=softLB "
            "85=SteamVR-RT 86=noEyeProjA/B 87=EyeProj+eyert 88=DEFAULT hitch+EyeProj "
            "89=cullSync; dualOptIn=%d) ok=%d fovSite=%d vsHook=%d pubProj=%d uploadFn=%d "
            "draw=%d",
            static_cast<int>(mode), dualOptIn ? 1 : 0, g_ok.load() ? 1 : 0, fovOk ? 1 : 0,
            okHook ? 1 : 0, okPub ? 1 : 0, okUpload ? 1 : 0, okDraw ? 1 : 0);
        Log("StereoRender: how-to — stereo=88 DEFAULT (EyeProj ON; dual 1/2 HOLD; "
            "eyert=1440); A/B 87=dual every/2048, 89=presence, 80=letterbox; kill 45; "
            "NEVER 86 default; skip 74; F8: 1cm=MOST fused");
        SetDrawnFovProven(false);
        Log("HmdLook: ENABLED DRAW-PATH (HMD orient + seated 6DoF frozen/ES + EyeToHead "
            "cached); UploadFn/PubProj also stamp look; F9 recenter; pose/seat spike reject");
        Log("PubProj: TEMPORAL HOLD 1×/self/ES (cached eye proj); never view+0x80");
        Log("PoseFreeze: BeginFrameHmdPoseSample + EMA reject (pos>8cm / fwdDot<0.90) + "
            "seat step clamp 8cm — jump fix path ON");
        if (IsConfigEyeRtLetterbox(mode) || IsCullSyncPresence(mode))
          Log("CullSync: CCam+PubProj target same HMD EyeProj FOV (aggressive); sidewalk "
              "hole possible if engine cull lags — keep EyeProj ON anyway");
        if (IsHitchCutEyeProj(mode))
          Log("HitchCut: dual 1/2 + HOLD + quieter EyeProj/log + eyert≤1440 default; "
              "device Reset releases eye RTs before recreate");
        if (IsAerPresenceMode(mode))
          Log("AERPose: Submit_TextureWithPose + engine L↔R AER (Mode76 quality)");
        Log("StereoRender: kill-switch - stereo=45");
      } else if (mode == StereoMode::HeadOwnedCamSpike) {
        Log("StereoRender: mode 45 HEAD-OWNED CAM SPIKE (Mode44 RT lock + post-CCam "
            "CopyMat HMD reapply; no collision/VS/replay change; not true stereo) ok=%d "
            "fovSite=%d",
            g_ok.load() ? 1 : 0, fovOk ? 1 : 0);
        Log("StereoRender: mode 45 MOTION-GUARD ON (fast direct-submit; thresholds "
            "rot>=0.8deg OR move>=1.2cm between L/R capture poses; Mode40/44 use "
            "1.5deg/2.0cm)");
        Log("StereoRender: kill-switch - stereo=44, 37, or 30 + delete "
            "gtaiv_dxvk_vr.fovadd");
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
          g_haveR ? 1 : 0, GetStereoSepMeters() * 100.f, GetLeanGain());
    return;
  }

    // Mode 30 / 35..43: pair-hold temporal only (device-VS dual proven dead — mode30dev=0).
    if (mode == StereoMode::PhaseDualDeviceVs || mode == StereoMode::FovRecomputeSite ||
        mode == StereoMode::FovRecomputeTrueCanvas || mode == StereoMode::FovCanvasComfort ||
        mode == StereoMode::AerPoseSubmit || mode == StereoMode::FovCanvasLowMotion ||
        mode == StereoMode::FovCanvasMotionGuard || mode == StereoMode::ReplayCallChainProbe ||
        mode == StereoMode::ReplayOwnerCountProbe ||
        mode == StereoMode::FovCanvasMotionGuardFast ||
        mode == StereoMode::FovCanvasMotionGuardRtLock || IsHeadOwnedCamFamily(mode)) {
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
      else if (mode == StereoMode::ViewConstCountProbe)
        Mode64OnEndScene(device);
      else if (mode == StereoMode::ReplayVtable178Log)
        Mode65OnEndScene(device);
      else if (mode == StereoMode::PublishSyncCountProbe)
        Mode66OnEndScene(device);
      else if (mode == StereoMode::ViewMatWriterCountProbe)
        Mode67OnEndScene(device);
      else if (mode == StereoMode::ReplayDispatchCountProbe)
        Mode68OnEndScene(device);
      else if (mode == StereoMode::ReplayDispatchMatPeek)
        Mode69OnEndScene(device);
      else if (mode == StereoMode::ReplayDispatchActivePeek)
        Mode70OnEndScene(device);
      else if (mode == StereoMode::ReplayDispatchTemporalInject)
        Mode71OnEndScene(device);
      else if (mode == StereoMode::VsConstTemporalInject)
        Mode72OnEndScene(device);
      else if (IsMode74Family(mode))
        Mode74OnEndScene(device);
      else if (mode == StereoMode::SameFrameVsConstDual) {
        // Captures happen in HookRoot1 dual; EndScene only ensures RTs exist.
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
        if (g_mode73DualDead.load())
          TemporalCapturePairHold(device);
      } else
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
      mode != StereoMode::FovRecomputeSite && mode != StereoMode::FovRecomputeTrueCanvas &&
      mode != StereoMode::FovCanvasComfort && mode != StereoMode::AerPoseSubmit &&
      mode != StereoMode::FovCanvasLowMotion && mode != StereoMode::FovCanvasMotionGuard &&
      mode != StereoMode::ReplayCallChainProbe && mode != StereoMode::ReplayOwnerCountProbe &&
      mode != StereoMode::FovCanvasMotionGuardFast &&
      mode != StereoMode::FovCanvasMotionGuardRtLock &&
      !IsHeadOwnedCamFamily(mode))
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

bool StereoMode74DualEnabled() {
  return g_mode74DualEnabled.load();
}

bool StereoMode74UploadFnArmed() {
  // Only report "owns IPD" when the hook is live AND has proven eye writes.
  // Armed-but-silent (0x2A1E10 rarely hit on CE — ReplayDispatch is the hot path)
  // must NOT disable CamMatrix IPD / Mode72 contentinj.
  return g_mode74UploadFnArmed.load() && !g_mode74UploadFnDead.load() &&
         g_mode74UploadFnEyeWrites.load() > 0;
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
        GetStereoMode() == StereoMode::ReplayOwnerCountProbe ? "Mode42" : "Mode41", s_sample, tid);
  }
}

}  // namespace asi
