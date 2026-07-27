#include "openvr_mono.h"
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
  const vr::EVRCompositorError poseErr =
      vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
  QueryPerformanceCounter(&poseT1);
  const double poseMs =
      (s_poseQpf.QuadPart > 0)
          ? (1000.0 * static_cast<double>(poseT1.QuadPart - poseT0.QuadPart) /
             static_cast<double>(s_poseQpf.QuadPart))
          : 0.0;
  PerfDebugOnPoseWait(poseMs, static_cast<int>(poseErr));
  StereoDualNotePoseMs(poseMs);
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

  StereoRenderOnDevice(device);
  UpdateGameFovFromDevice(device);
  TryApplyCoverMatchedFovAdd();  // Mode 123 once cover FOV known

  // Always ask — StereoTrySubmitEyes has the full mode + haveL/R gate. A second
  // mode list here went stale (mode 26 captured canvases but never submitted →
  // mono BB, 90 FPS, no fusion / no jumping).
  const bool stereoSubmitted = StereoTrySubmitEyes(device, interop);

  vr::EVRCompositorError eL = vr::VRCompositorError_None;
  vr::EVRCompositorError eR = vr::VRCompositorError_None;
  if (!stereoSubmitted) {
    // Mode 0: keep nullptr (user liked this FOV). Inset is for temporal stereo only.
    interop->LockSubmissionQueue();
    eL = vr::VRCompositor()->Submit(vr::Eye_Left, &texture, nullptr, vr::Submit_Default);
    eR = vr::VRCompositor()->Submit(vr::Eye_Right, &texture, nullptr, vr::Submit_Default);
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
  PollStereoScaleHotkey();

  // Head tracking: mouse-look after warmup. Engine FP cam after more stable frames.
  constexpr uint32_t kLookAfter = 120;
  constexpr uint32_t kCamAfter = 360;
  if (n > kLookAfter) {
    ApplyHmdMouseLook(poses, vr::k_unMaxTrackedDeviceCount);
    UpdateLookMove();  // stick yaw + optional HMD→ped heading (Mode120 forces ON)
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
