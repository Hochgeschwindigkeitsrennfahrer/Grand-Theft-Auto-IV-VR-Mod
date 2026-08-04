#include "openvr_mono.h"
#include "aim_decouple.h"
#include "cam_matrix.h"
#include "hmd_look.h"
#include "hmd_pose.h"
#include "log.h"
#include "look_move.h"
#include "ped_hide.h"
#include "perf_debug.h"
#include "stereo_config.h"
#include "stereo_dual.h"
#include "stereo_render.h"
#include "hud_layout.h"
#include "vr_display.h"
#include "vr_move.h"

#include "../../thirdparty/dxvk/d3d9_vk_interop.h"

#include <d3d9.h>
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <mutex>

#include <openvr.h>

namespace asi {
namespace {

std::mutex g_vrMu;
std::atomic<bool> g_vrReady{false};
std::atomic<bool> g_vrFailed{false};
std::atomic<bool> g_submitDisabled{false};
std::atomic<uint32_t> g_submitCount{0};
std::atomic<uint32_t> g_warmupFrames{0};

bool DisableFilePresent() {
  char path[MAX_PATH]{};
  HMODULE self = nullptr;
  GetModuleHandleExA(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCSTR>(&DisableFilePresent), &self);
  GetModuleFileNameA(self, path, MAX_PATH);
  char* slash = strrchr(path, '\\');
  if (!slash)
    slash = strrchr(path, '/');
  if (slash)
    slash[1] = 0;
  strcat_s(path, "gtaiv_dxvk_vr.disable");
  return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Close SteamVR system dashboard if open. Uses IsDashboardVisible + toggle URL
// (OpenVR has ShowDashboard, no Hide). ShellExecute only — never WaitGetPoses /
// Submit here (those stay on the EndScene thread).
void TryCloseSteamVrDashboard() {
  static std::atomic<int> s_attempts{0};
  if (s_attempts.load() >= 2)
    return;
  vr::IVROverlay* ov = vr::VROverlay();
  if (!ov || !ov->IsDashboardVisible())
    return;
  const int n = ++s_attempts;
  // Async; does not touch the compositor submit path.
  ShellExecuteA(nullptr, "open", "vrmonitor://debugcommands/system_dashboard_toggle",
                nullptr, nullptr, SW_HIDE);
  Log("OpenVR: dashboard visible — requested close via system_dashboard_toggle (#%d)", n);
}

bool EnsureOpenVrUnlocked() {
  if (g_vrReady.load())
    return true;
  if (g_vrFailed.load() || g_submitDisabled.load())
    return false;

  if (DisableFilePresent()) {
    Log("OpenVR: disabled via gtaiv_dxvk_vr.disable");
    g_submitDisabled = true;
    g_vrFailed = true;
    return false;
  }

  if (!vr::VR_IsRuntimeInstalled()) {
    Log("OpenVR: runtime not installed");
    g_vrFailed = true;
    return false;
  }
  if (!vr::VR_IsHmdPresent()) {
    Log("OpenVR: no HMD present — start SteamVR first");
    g_vrFailed = true;
    return false;
  }

  vr::EVRInitError err = vr::VRInitError_None;
  vr::IVRSystem* sys = vr::VR_Init(&err, vr::VRApplication_Scene);
  if (err != vr::VRInitError_None || !sys) {
    Log("OpenVR VR_Init FAILED: %s", vr::VR_GetVRInitErrorAsEnglishDescription(err));
    g_vrFailed = true;
    return false;
  }
  if (!vr::VRCompositor()) {
    Log("OpenVR: VRCompositor() null");
    vr::VR_Shutdown();
    g_vrFailed = true;
    return false;
  }

  Log("OpenVR: VR_Init OK (Scene) — warmup, then WaitGetPoses+Submit on EndScene");
  TryCloseSteamVrDashboard();
  LogVrDisplayInfo();
  g_warmupFrames = 90;
  g_vrReady = true;
  return true;
}

}  // namespace

bool InitOpenVrEarly() {
  if (g_submitDisabled.load() || DisableFilePresent()) {
    g_submitDisabled = true;
    Log("OpenVR: skip early init (disabled)");
    return false;
  }
  std::lock_guard<std::mutex> lock(g_vrMu);
  return EnsureOpenVrUnlocked();
}

std::atomic<unsigned> g_esSinceDualFrame{0};

unsigned VrEndScenesSinceDualFrame() {
  return g_esSinceDualFrame.load();
}

bool VrSubmitReady() {
  return g_vrReady.load() && !g_vrFailed.load() && !g_submitDisabled.load() &&
         g_warmupFrames.load() == 0;
}

// Mode 271: WaitGetPoses at the START of the AER walk. The one eye rendered
// this frame then uses this pose, and the pair is submitted at the walk's
// end — the canonical WaitGetPoses -> render -> Submit order, which EndScene
// (firing INSIDE origDrawWalk) can never provide.
bool VrBeginFrameFromDual() {
  if (!VrSubmitReady() || !vr::VRCompositor())
    return false;
  vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
  const vr::EVRCompositorError err =
      vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
  if (err == vr::VRCompositorError_DoNotHaveFocus)
    vr::VRCompositor()->CompositorBringToFront();
  UpdateHmdPose(poses, vr::k_unMaxTrackedDeviceCount);
  AimDecoupleOnPoses(poses, vr::k_unMaxTrackedDeviceCount);
  g_esSinceDualFrame.store(0);
  static uint32_t s_n = 0;
  if ((++s_n) <= 4 || (s_n % 900) == 0)
    Log("Mode271: VR frame begun from AER walk #%u (WaitGetPoses -> render -> Submit)", s_n);
  return true;
}

void TryMonoSubmit(IDirect3DDevice9* device) {
  if (!device || g_submitDisabled.load() || g_vrFailed.load())
    return;
  if (DisableFilePresent()) {
    g_submitDisabled = true;
    return;
  }
  if (!g_vrReady.load())
    return;

  if (g_warmupFrames.load() > 0) {
    g_warmupFrames.fetch_sub(1);
    return;
  }

  // Mode 271: the AER walk owns the VR frame (WaitGetPoses -> render -> Submit
  // from VrBeginFrameFromDual / StereoSubmitPairAtDualEnd). Skip the EndScene
  // pose wait + submit WHILE the walk is actually driving frames. WATCHDOG: if
  // the walk has not driven a frame for several EndScenes (menu, loading
  // screen, hook removed), fall back to the full legacy path here so the
  // compositor can never starve.
  bool doVrFrame = true;
  // True only while the watchdog below has taken the frame back from the walk.
  bool walkStalled = false;
  if (IsOursFp203DualDrivenVrFrame(GetStereoMode())) {
    const unsigned since = VrEndScenesSinceDualFrame();
    doVrFrame = (since >= 4u);
    walkStalled = doVrFrame;
    static uint32_t s_fb = 0;
    if (doVrFrame && ((++s_fb) <= 4 || (s_fb % 600) == 0))
      Log("Mode271: WATCHDOG — AER walk has not driven a VR frame for %u EndScenes; "
          "running the legacy EndScene path (#%u)", since, s_fb);
  }

  ID3D9VkInteropDevice* interop = nullptr;
  if (FAILED(device->QueryInterface(__uuidof(ID3D9VkInteropDevice),
                                    reinterpret_cast<void**>(&interop))) ||
      !interop)
    return;

  IDirect3DSurface9* bb = nullptr;
  if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
    interop->Release();
    return;
  }

  ID3D9VkInteropTexture* tex = nullptr;
  if (FAILED(bb->QueryInterface(__uuidof(ID3D9VkInteropTexture), reinterpret_cast<void**>(&tex))) ||
      !tex) {
    bb->Release();
    interop->Release();
    return;
  }

  VkImage image = nullptr;
  VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageCreateInfo info{};
  info.sType = 14;
  uint32_t qfiScratch = 0;
  info.queueFamilyIndexCount = 1;
  info.pQueueFamilyIndices = &qfiScratch;

  if (FAILED(tex->GetVulkanImageInfo(&image, &oldLayout, &info)) || !image) {
    tex->Release();
    bb->Release();
    interop->Release();
    return;
  }

  VkInstance instance = nullptr;
  VkPhysicalDevice phys = nullptr;
  VkDevice vkdev = nullptr;
  interop->GetVulkanHandles(&instance, &phys, &vkdev);

  VkQueue queue = nullptr;
  uint32_t qIndex = 0;
  uint32_t qFamily = 0;
  interop->GetSubmissionQueue(&queue, &qIndex, &qFamily);

  if (!instance || !phys || !vkdev || !queue) {
    tex->Release();
    bb->Release();
    interop->Release();
    return;
  }

  interop->FlushRenderingCommands();

  vr::VRVulkanTextureData_t vulkanData{};
  vulkanData.m_nImage = reinterpret_cast<uint64_t>(image);
  vulkanData.m_pDevice = reinterpret_cast<VkDevice_T*>(vkdev);
  vulkanData.m_pPhysicalDevice = reinterpret_cast<VkPhysicalDevice_T*>(phys);
  vulkanData.m_pInstance = reinterpret_cast<VkInstance_T*>(instance);
  vulkanData.m_pQueue = reinterpret_cast<VkQueue_T*>(queue);
  vulkanData.m_nQueueFamilyIndex = qFamily;
  vulkanData.m_nWidth = info.extent.width;
  vulkanData.m_nHeight = info.extent.height;
  vulkanData.m_nFormat = static_cast<uint32_t>(info.format);
  vulkanData.m_nSampleCount = info.samples ? static_cast<uint32_t>(info.samples) : 1u;

  vr::Texture_t texture{};
  texture.handle = &vulkanData;
  texture.eType = vr::TextureType_Vulkan;
  texture.eColorSpace = vr::ColorSpace_Gamma;

  // Same-thread OpenVR frame: WaitGetPoses then Submit L/R.
  // (Separate pose thread caused AlreadySubmitted=108 and freeze.)
  if (!vr::VRCompositor()->CanRenderScene())
    vr::VRCompositor()->CompositorBringToFront();

  vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
  static LARGE_INTEGER s_poseQpf{};
  if (s_poseQpf.QuadPart == 0)
    QueryPerformanceFrequency(&s_poseQpf);
  LARGE_INTEGER poseT0{}, poseT1{};
  QueryPerformanceCounter(&poseT0);
  // Mode 271: only take the BLOCKING wait on the EndScene that owns the VR
  // frame. On the others, still fill `poses` with a non-blocking read —
  // leaving it zero-initialised makes bPoseIsValid false, which drops
  // g_valid, which makes ApplyHmdToCam bail and hands the camera back to the
  // game (third-person flicker + dead head tracking every other frame).
  vr::EVRCompositorError poseErr = vr::VRCompositorError_None;
  if (doVrFrame) {
    poseErr = vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
  } else if (vr::VRSystem()) {
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.f, poses,
                                                    vr::k_unMaxTrackedDeviceCount);
  }
  QueryPerformanceCounter(&poseT1);
  const double poseMs =
      (s_poseQpf.QuadPart > 0)
          ? (1000.0 * static_cast<double>(poseT1.QuadPart - poseT0.QuadPart) /
             static_cast<double>(s_poseQpf.QuadPart))
          : 0.0;
  if (doVrFrame) {
    PerfDebugOnPoseWait(poseMs, static_cast<int>(poseErr));
    StereoDualNotePoseMs(poseMs);
  }
  if (poseErr == vr::VRCompositorError_DoNotHaveFocus) {
    vr::VRCompositor()->CompositorBringToFront();
    // One more async dashboard close if still open (max 2 total; no OpenVR
    // WaitGetPoses/Submit from any other thread).
    TryCloseSteamVrDashboard();
    PerfDebugVrNote("WaitGetPoses DoNotHaveFocus — CompositorBringToFront");
  } else if (poseErr != vr::VRCompositorError_None) {
    PerfDebugVrNote("WaitGetPoses err=%d", static_cast<int>(poseErr));
  }
  UpdateHmdPose(poses, vr::k_unMaxTrackedDeviceCount);
  AimDecoupleOnPoses(poses, vr::k_unMaxTrackedDeviceCount);

  StereoRenderOnDevice(device);
  UpdateGameFovFromDevice(device);
  TryApplyCoverMatchedFovAdd();  // Mode 123 once cover FOV known
  UpdateHudLayoutForVr();        // Mode 166 radar/status inward

  // Always ask — StereoTrySubmitEyes has the full mode + haveL/R gate. A second
  // mode list here went stale (mode 26 captured canvases but never submitted →
  // mono BB, 90 FPS, no fusion / no jumping).
  // Mode 271: the pair was already submitted at the END of the AER walk,
  // where the eye is captured AND fresh. Do not submit again here (and do not
  // fall back to the mono BB path) — UNLESS the watchdog above says the walk
  // has stalled, in which case re-submit the last completed pair from here so
  // the compositor never starves (a menu, a loading screen, a hitch). The
  // pair is a hold, but an honestly stamped hold is exactly what reprojection
  // is for.
  const bool aerHoldSubmit = IsAerSingleDraw(GetStereoMode()) && walkStalled;
  const bool stereoSubmitted =
      (IsOursFp203SubmitAtDualEnd(GetStereoMode()) && !aerHoldSubmit)
          ? true
          : (doVrFrame ? StereoTrySubmitEyes(device, interop) : true);
  if (aerHoldSubmit) {
    static uint32_t s_hold = 0;
    if ((++s_hold) <= 4 || (s_hold % 600) == 0)
      Log("AER: walk stalled — EndScene re-submitted the last completed pair (#%u ok=%d)",
          s_hold, stereoSubmitted ? 1 : 0);
  }

  vr::EVRCompositorError eL = vr::VRCompositorError_None;
  vr::EVRCompositorError eR = vr::VRCompositorError_None;
  if (!stereoSubmitted) {
    // Mode 0: nullptr bounds (user liked FOV). Mode 174/175: TextureBounds frame the
    // 16:9 BB so bottom-right phone sits higher / more inward in the HMD.
    const vr::VRTextureBounds_t* bounds = nullptr;
    vr::VRTextureBounds_t phoneLift{};
    if (IsOursFpBbPhone(GetStereoMode())) {
      // 174: lift only. 175: lift more + crop left so right-side phone moves inward.
      const bool nudge = IsOursFpBbPhoneNudge(GetStereoMode());
      phoneLift.uMin = nudge ? 0.14f : 0.f;
      phoneLift.uMax = 1.f;
      phoneLift.vMin = nudge ? 0.42f : 0.30f;
      phoneLift.vMax = 1.f;
      bounds = &phoneLift;
      static bool s_once = false;
      if (!s_once) {
        s_once = true;
        Log("Mode%d: BB Submit bounds uMin=%.2f vMin=%.2f (phone framing); stereo=0 path",
            static_cast<int>(GetStereoMode()), phoneLift.uMin, phoneLift.vMin);
      }
    }
    interop->LockSubmissionQueue();
    eL = vr::VRCompositor()->Submit(vr::Eye_Left, &texture, bounds, vr::Submit_Default);
    eR = vr::VRCompositor()->Submit(vr::Eye_Right, &texture, bounds, vr::Submit_Default);
    interop->ReleaseSubmissionQueue();
  }

  const uint32_t n = ++g_submitCount;
  if (n <= 10 || (n % 60) == 0) {
    Log("MonoSubmit #%u %ux%u fmt=%u pose=%d errL=%d errR=%d can=%d stereo=%d", n,
        vulkanData.m_nWidth, vulkanData.m_nHeight, vulkanData.m_nFormat, static_cast<int>(poseErr),
        static_cast<int>(eL), static_cast<int>(eR),
        vr::VRCompositor()->CanRenderScene() ? 1 : 0, stereoSubmitted ? 1 : 0);
  }
  if (eL != vr::VRCompositorError_None || eR != vr::VRCompositorError_None) {
    PerfDebugVrNote("Submit errL=%d errR=%d stereo=%d n=%u", static_cast<int>(eL),
                    static_cast<int>(eR), stereoSubmitted ? 1 : 0, n);
  }
  // Sampled perf/mem (~1.5–2s) — no per-frame I/O, no FPS HUD.
  PerfDebugOnPresent(stereoSubmitted, false, StereoDualHoldActive(), 0);

  PollRecenterHotkey();
  PollCamHotkeys();
  PollIpdScaleHotkey();
  PollWorldScaleHotkey();
  PollTrueWorldScaleHotkey();
  PollStereoScaleHotkey();
  PollVrResHotkey();

  // Head tracking: mouse-look after warmup. Engine FP cam after more stable frames.
  constexpr uint32_t kLookAfter = 120;
  constexpr uint32_t kCamAfter = 360;
  if (n > kLookAfter) {
    ApplyHmdMouseLook(poses, vr::k_unMaxTrackedDeviceCount);
    UpdateLookMove();  // stick yaw + optional HMD→ped heading (Mode120 forces ON)
    UpdateAimDecouple();  // aimmode=0 inert; =1 probe log only
  }
  if (n == kCamAfter) {
    if (InstallCamMatrixHooks()) {
      SetCamMatrixGameplayActive(true);
      LogVrDisplayInfo();
      ReloadStereoMode();
      InstallStereoRenderHooks();
      // Warm PedHide resolve once on game thread after FP arm (also called from CopyMat).
      UpdatePedHeadHide();
      Log("CamMatrix: armed after %u submits (overrides mouse-look)", kCamAfter);
    }
  }

  tex->Release();
  bb->Release();
  interop->Release();
}

void OpenVrShutdown() {
  std::lock_guard<std::mutex> lock(g_vrMu);
  if (g_vrReady.exchange(false)) {
    vr::VR_Shutdown();
    Log("OpenVR: VR_Shutdown");
  }
}

}  // namespace asi
