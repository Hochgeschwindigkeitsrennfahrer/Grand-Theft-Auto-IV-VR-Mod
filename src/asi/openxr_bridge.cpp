#include "openxr_bridge.h"

#include "cam_matrix.h"
#include "hmd_pose.h"
#include "log.h"
#include "openxr_controller.h"
#include "openxr_pose_client.h"
#include "ped_hide.h"
#include "stereo_config.h"
#include "stereo_render.h"
#include "ui_state.h"
#include "vr_display.h"
#include "vr_move.h"
#include "../bridge/gtaiv_xr_frame_bridge.h"

#include <windows.h>

#include <d3d9.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "../../thirdparty/dxvk/d3d9_vk_interop.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>

namespace asi
{
namespace
{
using Microsoft::WRL::ComPtr;
using gtaiv_xr_bridge::FrameBridge;
using gtaiv_xr_bridge::PixelFormat;

constexpr VkExternalMemoryHandleTypeFlagBits MemoryHandleType =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
constexpr VkExternalSemaphoreHandleTypeFlagBits FenceHandleType =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;

std::string moduleDirectory()
{
    char path[MAX_PATH] {};
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&moduleDirectory),
        &self);
    const DWORD length = GetModuleFileNameA(self, path, MAX_PATH);
    std::string result(path, length);
    const size_t slash = result.find_last_of("\\/");
    if (slash != std::string::npos)
        result.resize(slash + 1u);
    return result;
}

uint64_t packLuid(const LUID& luid)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32u)
        | static_cast<uint64_t>(luid.LowPart);
}

uint64_t handleValue(HANDLE handle)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
}

bool sameLuid(const LUID& left, const LUID& right)
{
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

DXGI_FORMAT dxgiFormat(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

PixelFormat protocolFormat(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_B8G8R8A8_UNORM:
            return PixelFormat::B8G8R8A8Unorm;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return PixelFormat::R8G8B8A8Unorm;
        default:
            return PixelFormat::Unknown;
    }
}

PixelFormat protocolFormat(D3DFORMAT format)
{
    switch (format)
    {
        case D3DFMT_A8R8G8B8:
            return PixelFormat::B8G8R8A8Unorm;
        case D3DFMT_X8R8G8B8:
            return PixelFormat::B8G8R8X8Unorm;
        default:
            return PixelFormat::Unknown;
    }
}

bool sameComIdentity(IUnknown* left, IUnknown* right)
{
    if (!left || !right)
        return false;
    ComPtr<IUnknown> leftIdentity;
    ComPtr<IUnknown> rightIdentity;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&leftIdentity)))
        && SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&rightIdentity)))
        && leftIdentity.Get() == rightIdentity.Get();
}

struct PairLease
{
    ~PairLease()
    {
        StereoReleaseOpenXrPair(&pair);
    }
    OpenXrStereoPair pair {};
};

struct VulkanFunctions
{
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2
        getPhysicalDeviceImageFormatProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties
        getPhysicalDeviceExternalSemaphoreProperties = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkGetMemoryWin32HandlePropertiesKHR getMemoryWin32HandleProperties = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkImportSemaphoreWin32HandleKHR importSemaphoreWin32Handle = nullptr;
    PFN_vkGetSemaphoreCounterValue getSemaphoreCounterValue = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer resetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;

    template <typename T>
    bool loadInstance(T& output, VkInstance instance, const char* name)
    {
        output = reinterpret_cast<T>(getInstanceProcAddr(instance, name));
        return output != nullptr;
    }

    template <typename T>
    bool loadDevice(T& output, VkDevice device, const char* name)
    {
        output = reinterpret_cast<T>(getDeviceProcAddr(device, name));
        return output != nullptr;
    }

    bool initialize(VkInstance instance, VkDevice device)
    {
        HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
        if (!loader)
            loader = LoadLibraryW(L"vulkan-1.dll");
        if (!loader)
            return false;

        getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(loader, "vkGetInstanceProcAddr"));
        if (!getInstanceProcAddr)
            return false;
        getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            getInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
        if (!getDeviceProcAddr)
            return false;

        bool ok =
            loadInstance(
                getPhysicalDeviceProperties2,
                instance,
                "vkGetPhysicalDeviceProperties2")
            && loadInstance(
                getPhysicalDeviceMemoryProperties,
                instance,
                "vkGetPhysicalDeviceMemoryProperties")
            && loadInstance(
                getPhysicalDeviceImageFormatProperties2,
                instance,
                "vkGetPhysicalDeviceImageFormatProperties2")
            && loadInstance(
                getPhysicalDeviceExternalSemaphoreProperties,
                instance,
                "vkGetPhysicalDeviceExternalSemaphoreProperties")
            && loadDevice(createImage, device, "vkCreateImage")
            && loadDevice(destroyImage, device, "vkDestroyImage")
            && loadDevice(
                getImageMemoryRequirements,
                device,
                "vkGetImageMemoryRequirements")
            && loadDevice(
                getMemoryWin32HandleProperties,
                device,
                "vkGetMemoryWin32HandlePropertiesKHR")
            && loadDevice(allocateMemory, device, "vkAllocateMemory")
            && loadDevice(freeMemory, device, "vkFreeMemory")
            && loadDevice(bindImageMemory, device, "vkBindImageMemory")
            && loadDevice(createSemaphore, device, "vkCreateSemaphore")
            && loadDevice(destroySemaphore, device, "vkDestroySemaphore")
            && loadDevice(
                importSemaphoreWin32Handle,
                device,
                "vkImportSemaphoreWin32HandleKHR")
            && loadDevice(createCommandPool, device, "vkCreateCommandPool")
            && loadDevice(destroyCommandPool, device, "vkDestroyCommandPool")
            && loadDevice(
                allocateCommandBuffers,
                device,
                "vkAllocateCommandBuffers")
            && loadDevice(
                resetCommandBuffer,
                device,
                "vkResetCommandBuffer")
            && loadDevice(
                beginCommandBuffer,
                device,
                "vkBeginCommandBuffer")
            && loadDevice(endCommandBuffer, device, "vkEndCommandBuffer")
            && loadDevice(
                cmdPipelineBarrier,
                device,
                "vkCmdPipelineBarrier")
            && loadDevice(cmdCopyImage, device, "vkCmdCopyImage")
            && loadDevice(queueSubmit, device, "vkQueueSubmit");
        if (!ok)
            return false;

        if (!loadDevice(
                getSemaphoreCounterValue,
                device,
                "vkGetSemaphoreCounterValue"))
        {
            loadDevice(
                getSemaphoreCounterValue,
                device,
                "vkGetSemaphoreCounterValueKHR");
        }
        return getSemaphoreCounterValue != nullptr;
    }
};

struct SourceEye
{
    ComPtr<IDirect3DSurface9> surface;
    ComPtr<ID3D9VkInteropTexture> interopTexture;
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    uint32_t queueFamilyScratch = 0u;
};

struct SharedEye
{
    ComPtr<ID3D11Texture2D> texture;
    HANDLE sharedHandle = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct SharedSlot
{
    std::array<SharedEye, gtaiv_xr_bridge::EyeCount> eyes;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    uint64_t transactionId = 0u;
    bool importedImageHasExternalContents = true;
};

class OpenXrFrameProducer
{
public:
    void heartbeat()
    {
        publishHeartbeat();
    }

    void publish(IDirect3DDevice9* gameDevice)
    {
        if (permanentlyDisabled_ || !gameDevice)
            return;

        const UiPresentationState uiState = GetUiPresentationState();
        const bool immersiveMono =
            !uiState
            && GetStereoMode() == StereoMode::OpenXrImmersiveMono;
        PairLease lease;
        if (!StereoAcquireOpenXrPair(&lease.pair))
        {
            if (!uiState)
                publishHeartbeat();
            return;
        }
        if (!lease.pair.poseStamped
            || lease.pair.eyes[0] == lease.pair.eyes[1])
        {
            logRateLimited(
                "OpenXRBridge: waiting for a pose-stamped, non-aliased L/R pair",
                lastPairWaitLogTick_);
            publishHeartbeat();
            return;
        }
        if (static_cast<bool>(uiState) != lease.pair.uiQuad
            || (!uiState
                && immersiveMono != lease.pair.immersiveMono))
        {
            logRateLimited(
                "OpenXRBridge: captured frame route does not match current "
                "menu/game state; old transaction held",
                lastRouteWaitLogTick_);
            publishHeartbeat();
            return;
        }
        if (!uiState && !lease.pair.sameSimulationTick)
        {
            logRateLimited(
                "OpenXRBridge: world frame is not one simulation tick/pose; "
                "GPU transaction withheld",
                lastPairWaitLogTick_);
            publishHeartbeat();
            return;
        }
        if (!uiState
            && !immersiveMono
            && !lease.pair.verifiedWvpStereo
            && !lease.pair.verifiedDrawSceneStereo)
        {
            logRateLimited(
                "OpenXRBridge: world pair lacks validated same-frame proof; "
                "GPU transaction withheld",
                lastPairWaitLogTick_);
            publishHeartbeat();
            return;
        }
        if (lease.pair.pairId == lastPairId_)
        {
            if (!uiState)
                publishHeartbeat();
            return;
        }

        const bool cpuMailboxRoute =
            GetStereoMode() == StereoMode::OpenXrImmersiveMono
            || GetStereoMode() == StereoMode::OpenXrDrawSceneStereo;
        if (cpuMailboxRoute)
        {
            if (!ready_)
            {
                if (!initializeCpuMailbox(gameDevice, lease.pair))
                {
                    disable("FNVVR-style CPU frame mailbox initialization failed");
                    return;
                }
            }
            else if (!cpuMailboxMode_
                     || !sameComIdentity(gameDevice_.Get(), gameDevice)
                     || width_ != lease.pair.width
                     || height_ != lease.pair.height)
            {
                disable("CPU mailbox game device or capture size changed");
                return;
            }
            if (!publishCpuMailboxFrame(
                    gameDevice,
                    lease.pair,
                    uiState,
                    immersiveMono))
            {
                logRateLimited(
                    "OpenXRBridge: CPU mailbox capture unavailable; "
                    "last complete frame retained",
                    lastCpuFailureLogTick_);
                publishHeartbeat();
            }
            return;
        }

        std::array<SourceEye, gtaiv_xr_bridge::EyeCount> source;
        if (!querySourceEyes(gameDevice, lease.pair, source))
        {
            disable("DXVK eye-image query failed");
            return;
        }

        if (!ready_)
        {
            if (!initialize(gameDevice, lease.pair, source))
            {
                disable("D3D11-NT to Vulkan import initialization failed");
                return;
            }
        }
        else if (!sameComIdentity(gameDevice_.Get(), gameDevice)
                 || width_ != lease.pair.width
                 || height_ != lease.pair.height
                 || vkFormat_ != source[0].info.format)
        {
            disable("game device, eye size, or eye format changed");
            return;
        }

        publishHeartbeat();
        const uint32_t slotIndex = findReusableSlot();
        if (slotIndex >= gtaiv_xr_bridge::SlotCount)
        {
            logRateLimited(
                "OpenXRBridge: all GPU slots await the x64 host release fence",
                lastBusyLogTick_);
            return;
        }

        const bool singleEyeTransport =
            uiState || lease.pair.immersiveMono;
        const uint32_t copyEyeCount =
            singleEyeTransport ? 1u : gtaiv_xr_bridge::EyeCount;
        const uint64_t transaction = nextTransaction_ + 1u;
        if (transaction == 0u
            || !submitCopy(
                slotIndex,
                transaction,
                source,
                copyEyeCount))
        {
            disable("Vulkan stereo copy submission failed");
            return;
        }

        nextTransaction_ = transaction;
        currentSlot_ = slotIndex;
        slots_[slotIndex].transactionId = transaction;
        lastPair_ = lease.pair;
        // The retained descriptor copy must not own the AddRef'd textures.
        lastPair_.eyes[0] = nullptr;
        lastPair_.eyes[1] = nullptr;
        lastPairId_ = lease.pair.pairId;
        lastPresentationMode_ = uiState
            ? gtaiv_xr_bridge::PresentationMode::UiQuad
            : lease.pair.immersiveMono
                ? gtaiv_xr_bridge::PresentationMode::WorldMono
                : gtaiv_xr_bridge::PresentationMode::WorldStereo;
        lastUiReasonFlags_ = uiState.reasonFlags;
        lastUiEye_ = singleEyeTransport
            ? 0u
            : lease.pair.sourceFrameId[1] > lease.pair.sourceFrameId[0]
                ? 1u
                : 0u;
        publishDescriptor();

        if (transaction == 1u || transaction % 120u == 0u)
        {
            Log(
                "OpenXRBridge: frame transaction=%llu slot=%u pair=%llu "
                "presentation=%s sameTick=%d wvpProof=%d "
                "pose=%llu/%llu source=%llu/%llu",
                static_cast<unsigned long long>(transaction),
                slotIndex,
                static_cast<unsigned long long>(lease.pair.pairId),
                lease.pair.immersiveMono
                    ? "world-immersive-mono"
                    : lease.pair.uiQuad
                        ? "stationary-ui-quad"
                        : "world-stereo",
                lease.pair.sameSimulationTick ? 1 : 0,
                lease.pair.verifiedWvpStereo ? 1 : 0,
                static_cast<unsigned long long>(lease.pair.poseSequence[0]),
                static_cast<unsigned long long>(lease.pair.poseSequence[1]),
                static_cast<unsigned long long>(lease.pair.sourceFrameId[0]),
                static_cast<unsigned long long>(lease.pair.sourceFrameId[1]));
        }
    }

    void announceStopped()
    {
        if (shared_)
        {
            FrameBridge stopped {};
            stopped.magic = gtaiv_xr_bridge::Magic;
            stopped.version = gtaiv_xr_bridge::Version;
            stopped.structBytes = sizeof(FrameBridge);
            stopped.slotCount = gtaiv_xr_bridge::SlotCount;
            stopped.producerPid = GetCurrentProcessId();
            stopped.producerEpoch = producerEpoch_;
            stopped.resourceSetId = resourceSetId_;
            stopped.heartbeatTickMs = GetTickCount64();
            stopped.mappingBytes = sizeof(FrameBridge);
            commit(stopped);
            UnmapViewOfFile(shared_);
            shared_ = nullptr;
        }
        if (mapping_)
        {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        if (cpuMailbox_)
        {
            UnmapViewOfFile(cpuMailbox_);
            cpuMailbox_ = nullptr;
        }
        if (cpuFrameMapping_)
        {
            CloseHandle(cpuFrameMapping_);
            cpuFrameMapping_ = nullptr;
        }
        // This function is called from DLL_PROCESS_DETACH. Do not wait on the
        // DXVK queue or tear down imported GPU objects under the loader lock;
        // process teardown releases those process-lifetime objects.
        ready_ = false;
    }

private:
    uint8_t* cpuSlotPixels(uint32_t slot, uint32_t eye) const
    {
        if (!cpuMailbox_
            || slot >= gtaiv_xr_bridge::SlotCount
            || eye >= gtaiv_xr_bridge::CpuFrameEyeCount)
        {
            return nullptr;
        }
        return reinterpret_cast<uint8_t*>(cpuMailbox_)
            + sizeof(gtaiv_xr_bridge::CpuFrameMailbox)
            + static_cast<size_t>(
                slot * gtaiv_xr_bridge::CpuFrameSlotBytes)
            + static_cast<size_t>(
                eye * gtaiv_xr_bridge::CpuFrameEyeBytes);
    }

    bool createCpuFrameMapping()
    {
        static_assert(
            gtaiv_xr_bridge::CpuFrameMappingBytes <= MAXDWORD,
            "CPU frame mailbox exceeds Win32 mapping size");
        cpuFrameMapping_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0u,
            static_cast<DWORD>(
                gtaiv_xr_bridge::CpuFrameMappingBytes),
            gtaiv_xr_bridge::CpuFrameMappingName);
        if (!cpuFrameMapping_)
        {
            Log(
                "OpenXRBridge: CreateFileMapping(CPU frame) failed win32=%lu",
                static_cast<unsigned long>(GetLastError()));
            return false;
        }
        cpuMailbox_ =
            static_cast<gtaiv_xr_bridge::CpuFrameMailbox*>(
                MapViewOfFile(
                    cpuFrameMapping_,
                    FILE_MAP_READ | FILE_MAP_WRITE,
                    0u,
                    0u,
                    static_cast<SIZE_T>(
                        gtaiv_xr_bridge::CpuFrameMappingBytes)));
        if (!cpuMailbox_)
        {
            Log(
                "OpenXRBridge: MapViewOfFile(CPU frame) failed win32=%lu",
                static_cast<unsigned long>(GetLastError()));
            return false;
        }
        std::memset(
            cpuMailbox_,
            0,
            sizeof(gtaiv_xr_bridge::CpuFrameMailbox));
        cpuMailbox_->magic = gtaiv_xr_bridge::CpuFrameMagic;
        cpuMailbox_->version = gtaiv_xr_bridge::CpuFrameVersion;
        cpuMailbox_->headerBytes =
            sizeof(gtaiv_xr_bridge::CpuFrameMailbox);
        cpuMailbox_->slotCount = gtaiv_xr_bridge::SlotCount;
        cpuMailbox_->maxWidth =
            gtaiv_xr_bridge::CpuFrameMaxWidth;
        cpuMailbox_->maxHeight =
            gtaiv_xr_bridge::CpuFrameMaxHeight;
        cpuMailbox_->bytesPerPixel =
            gtaiv_xr_bridge::CpuFrameBytesPerPixel;
        cpuMailbox_->eyeCount = gtaiv_xr_bridge::CpuFrameEyeCount;
        cpuMailbox_->mappingBytes =
            gtaiv_xr_bridge::CpuFrameMappingBytes;
        return true;
    }

    bool initializeCpuMailbox(
        IDirect3DDevice9* gameDevice,
        const OpenXrStereoPair& pair)
    {
        if (!gameDevice
            || !pair.eyes[0]
            || (pair.verifiedDrawSceneStereo && !pair.eyes[1])
            || pair.width == 0u
            || pair.height == 0u
            || pair.width > gtaiv_xr_bridge::CpuFrameMaxWidth
            || pair.height > gtaiv_xr_bridge::CpuFrameMaxHeight)
        {
            return false;
        }

        ComPtr<IDirect3DSurface9> source;
        D3DSURFACE_DESC description {};
        if (FAILED(pair.eyes[0]->GetSurfaceLevel(0, &source))
            || !source
            || FAILED(source->GetDesc(&description))
            || description.Width != pair.width
            || description.Height != pair.height)
        {
            return false;
        }
        cpuProtocolFormat_ = protocolFormat(description.Format);
        if (cpuProtocolFormat_ == PixelFormat::Unknown)
        {
            Log(
                "OpenXRBridge: CPU mailbox rejects D3D9 format=%u",
                static_cast<unsigned>(description.Format));
            return false;
        }
        for (ComPtr<IDirect3DSurface9>& readback : cpuReadbacks_)
        {
            if (FAILED(gameDevice->CreateOffscreenPlainSurface(
                    pair.width,
                    pair.height,
                    description.Format,
                    D3DPOOL_SYSTEMMEM,
                    &readback,
                    nullptr))
                || !readback)
            {
                Log(
                    "OpenXRBridge: CPU mailbox readback surface creation failed "
                    "size=%ux%u format=%u",
                    pair.width,
                    pair.height,
                    static_cast<unsigned>(description.Format));
                return false;
            }
        }

        gameDevice_ = gameDevice;
        width_ = pair.width;
        height_ = pair.height;
        cpuD3dFormat_ = description.Format;
        if (!createCpuFrameMapping() || !createMapping())
            return false;

        LARGE_INTEGER counter {};
        QueryPerformanceCounter(&counter);
        producerEpoch_ =
            static_cast<uint64_t>(counter.QuadPart)
            ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32u)
            ^ GetTickCount64();
        if (producerEpoch_ == 0u)
            producerEpoch_ = 1u;
        resourceSetId_ =
            producerEpoch_
            ^ (static_cast<uint64_t>(width_) << 32u)
            ^ static_cast<uint64_t>(height_)
            ^ 0x4350554d41494cULL;
        if (resourceSetId_ == 0u)
            resourceSetId_ = 1u;
        resourceGeneration_ = 1u;
        cpuMailboxMode_ = true;
        ready_ = true;
        publishDescriptor();
        Log(
            "OpenXRBridge: READY FNVVR-style advancing CPU mailbox "
            "capture=%ux%u eyes=%u slots=%u (no host fence on GTA/DXVK queue)",
            width_,
            height_,
            gtaiv_xr_bridge::CpuFrameEyeCount,
            gtaiv_xr_bridge::SlotCount);
        return true;
    }

    bool publishCpuMailboxFrame(
        IDirect3DDevice9* gameDevice,
        const OpenXrStereoPair& pair,
        const UiPresentationState& uiState,
        bool immersiveMono)
    {
        if (!cpuMailboxMode_
            || !cpuMailbox_
            || !gameDevice
            || !pair.eyes[0])
        {
            return false;
        }

        const bool distinctStereo =
            !uiState && !immersiveMono && pair.verifiedDrawSceneStereo;
        const uint32_t sourceEyeCount = distinctStereo
            ? gtaiv_xr_bridge::EyeCount
            : 1u;
        if (sourceEyeCount == gtaiv_xr_bridge::EyeCount && !pair.eyes[1])
            return false;

        std::array<ComPtr<IDirect3DSurface9>, gtaiv_xr_bridge::EyeCount>
            sources;
        std::array<D3DLOCKED_RECT, gtaiv_xr_bridge::EyeCount> locked {};
        std::array<bool, gtaiv_xr_bridge::EyeCount> isLocked {};
        const auto unlockAll = [&]() {
            for (uint32_t eye = 0u; eye < sourceEyeCount; ++eye)
            {
                if (isLocked[eye])
                {
                    cpuReadbacks_[eye]->UnlockRect();
                    isLocked[eye] = false;
                }
            }
        };

        const uint32_t rowBytes =
            width_ * gtaiv_xr_bridge::CpuFrameBytesPerPixel;
        LARGE_INTEGER start {};
        LARGE_INTEGER afterReadback {};
        LARGE_INTEGER finished {};
        QueryPerformanceCounter(&start);
        for (uint32_t eye = 0u; eye < sourceEyeCount; ++eye)
        {
            D3DSURFACE_DESC description {};
            if (!cpuReadbacks_[eye]
                || FAILED(pair.eyes[eye]->GetSurfaceLevel(
                    0u, &sources[eye]))
                || !sources[eye]
                || FAILED(sources[eye]->GetDesc(&description))
                || description.Width != width_
                || description.Height != height_
                || description.Format != cpuD3dFormat_
                || FAILED(gameDevice->GetRenderTargetData(
                    sources[eye].Get(), cpuReadbacks_[eye].Get())))
            {
                unlockAll();
                return false;
            }
            if (FAILED(cpuReadbacks_[eye]->LockRect(
                    &locked[eye], nullptr, D3DLOCK_READONLY)))
            {
                unlockAll();
                return false;
            }
            isLocked[eye] = true;
            if (!locked[eye].pBits
                || locked[eye].Pitch < static_cast<INT>(rowBytes))
            {
                unlockAll();
                return false;
            }
        }
        QueryPerformanceCounter(&afterReadback);

        const uint32_t slot = nextSlot_;
        nextSlot_ =
            (nextSlot_ + 1u) % gtaiv_xr_bridge::SlotCount;
        volatile LONG* slotSequence =
            reinterpret_cast<volatile LONG*>(
                &cpuMailbox_->slotSequence[slot]);
        LONG current =
            InterlockedCompareExchange(slotSequence, 0, 0);
        LONG writing = (current & 1) ? current + 2 : current + 1;
        InterlockedExchange(slotSequence, writing);
        MemoryBarrier();

        for (uint32_t eye = 0u; eye < sourceEyeCount; ++eye)
        {
            uint8_t* destination = cpuSlotPixels(slot, eye);
            if (!destination)
            {
                unlockAll();
                InterlockedExchange(slotSequence, writing + 1);
                return false;
            }
            const auto* sourceBytes =
                static_cast<const uint8_t*>(locked[eye].pBits);
            for (uint32_t y = 0u; y < height_; ++y)
            {
                std::memcpy(
                    destination + static_cast<size_t>(y) * rowBytes,
                    sourceBytes
                        + static_cast<size_t>(y)
                            * static_cast<size_t>(locked[eye].Pitch),
                    rowBytes);
            }
        }
        unlockAll();

        const uint64_t transaction = nextTransaction_ + 1u;
        if (transaction == 0u)
            return false;
        cpuMailbox_->slotTransactionId[slot] = transaction;
        MemoryBarrier();
        InterlockedExchange(slotSequence, writing + 1);

        nextTransaction_ = transaction;
        currentSlot_ = slot;
        lastPair_ = pair;
        lastPair_.eyes[0] = nullptr;
        lastPair_.eyes[1] = nullptr;
        lastPairId_ = pair.pairId;
        lastPresentationMode_ = uiState
            ? gtaiv_xr_bridge::PresentationMode::UiQuad
            : immersiveMono
                ? gtaiv_xr_bridge::PresentationMode::WorldMono
                : gtaiv_xr_bridge::PresentationMode::WorldStereo;
        lastUiReasonFlags_ = uiState.reasonFlags;
        lastUiEye_ = 0u;
        publishDescriptor();
        QueryPerformanceCounter(&finished);

        ++cpuCaptureCount_;
        if (cpuCaptureCount_ <= 4u
            || cpuCaptureCount_ % 120u == 0u
            || lastLoggedCpuPresentation_ != lastPresentationMode_)
        {
            LARGE_INTEGER frequency {};
            QueryPerformanceFrequency(&frequency);
            const double ticksPerMs =
                frequency.QuadPart > 0
                    ? static_cast<double>(frequency.QuadPart) / 1000.0
                    : 1.0;
            Log(
                "OpenXRBridge: CPU mailbox transaction=%llu slot=%u "
                "presentation=%s eyes=%u capture=%ux%u "
                "readbackMs=%.2f copyMs=%.2f",
                static_cast<unsigned long long>(transaction),
                slot,
                lastPresentationMode_
                        == gtaiv_xr_bridge::PresentationMode::UiQuad
                    ? "stationary-ui-quad"
                    : lastPresentationMode_
                            == gtaiv_xr_bridge::PresentationMode::WorldStereo
                        ? "world-drawscene-stereo"
                        : "world-immersive-mono",
                sourceEyeCount,
                width_,
                height_,
                static_cast<double>(
                    afterReadback.QuadPart - start.QuadPart)
                    / ticksPerMs,
                static_cast<double>(
                    finished.QuadPart - afterReadback.QuadPart)
                    / ticksPerMs);
            lastLoggedCpuPresentation_ = lastPresentationMode_;
        }
        return true;
    }

    bool querySourceEyes(
        IDirect3DDevice9* gameDevice,
        const OpenXrStereoPair& pair,
        std::array<SourceEye, gtaiv_xr_bridge::EyeCount>& output)
    {
        if (!interop_)
        {
            if (FAILED(gameDevice->QueryInterface(
                    __uuidof(ID3D9VkInteropDevice),
                    reinterpret_cast<void**>(interop_.GetAddressOf())))
                || !interop_)
            {
                Log("OpenXRBridge: ID3D9VkInteropDevice unavailable");
                return false;
            }
        }

        for (uint32_t eye = 0u; eye < gtaiv_xr_bridge::EyeCount; ++eye)
        {
            SourceEye& value = output[eye];
            if (!pair.eyes[eye]
                || FAILED(pair.eyes[eye]->GetSurfaceLevel(0, &value.surface))
                || !value.surface
                || FAILED(value.surface->QueryInterface(
                    __uuidof(ID3D9VkInteropTexture),
                    reinterpret_cast<void**>(
                        value.interopTexture.GetAddressOf())))
                || !value.interopTexture)
            {
                Log("OpenXRBridge: eye %u lacks DXVK texture interop", eye);
                return false;
            }

            value.info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            value.info.queueFamilyIndexCount = 1u;
            value.info.pQueueFamilyIndices = &value.queueFamilyScratch;
            if (FAILED(value.interopTexture->GetVulkanImageInfo(
                    &value.image,
                    &value.layout,
                    &value.info))
                || value.image == VK_NULL_HANDLE
                || value.info.extent.width != pair.width
                || value.info.extent.height != pair.height
                || value.info.extent.depth != 1u
                || value.info.mipLevels != 1u
                || value.info.arrayLayers != 1u
                || value.info.samples != VK_SAMPLE_COUNT_1_BIT
                || (value.info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0u)
            {
                Log("OpenXRBridge: eye %u Vulkan image contract rejected", eye);
                return false;
            }
        }
        if (output[0].image == output[1].image
            || output[0].info.format != output[1].info.format)
        {
            Log("OpenXRBridge: L/R Vulkan images alias or use different formats");
            return false;
        }
        return true;
    }

    bool initialize(
        IDirect3DDevice9* gameDevice,
        const OpenXrStereoPair& pair,
        const std::array<SourceEye, gtaiv_xr_bridge::EyeCount>& source)
    {
        gameDevice_ = gameDevice;
        width_ = pair.width;
        height_ = pair.height;
        vkFormat_ = source[0].info.format;
        dxgiFormat_ = dxgiFormat(vkFormat_);
        if (dxgiFormat_ == DXGI_FORMAT_UNKNOWN
            || protocolFormat(vkFormat_) == PixelFormat::Unknown)
        {
            Log(
                "OpenXRBridge: Vulkan format %d is not a supported D3D11 color format",
                static_cast<int>(vkFormat_));
            return false;
        }

        interop_->GetVulkanHandles(
            &vkInstance_,
            &vkPhysicalDevice_,
            &vkDevice_);
        interop_->GetSubmissionQueue(
            &vkQueue_,
            &vkQueueIndex_,
            &vkQueueFamily_);
        if (vkInstance_ == VK_NULL_HANDLE
            || vkPhysicalDevice_ == VK_NULL_HANDLE
            || vkDevice_ == VK_NULL_HANDLE
            || vkQueue_ == VK_NULL_HANDLE)
        {
            Log("OpenXRBridge: DXVK returned incomplete Vulkan handles");
            return false;
        }
        if (!vk_.initialize(vkInstance_, vkDevice_))
        {
            Log("OpenXRBridge: required Vulkan entry points are not enabled");
            return false;
        }
        if (!createExactAdapterD3D11Device())
            return false;
        if (!checkExternalCapabilities())
            return false;
        if (!createSharedFences())
            return false;
        if (!createSharedSlots())
            return false;
        if (!createCommandPool())
            return false;
        if (!createMapping())
            return false;

        LARGE_INTEGER counter {};
        QueryPerformanceCounter(&counter);
        producerEpoch_ =
            static_cast<uint64_t>(counter.QuadPart)
            ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32u)
            ^ GetTickCount64();
        if (producerEpoch_ == 0u)
            producerEpoch_ = 1u;
        resourceSetId_ =
            producerEpoch_
            ^ (static_cast<uint64_t>(width_) << 32u)
            ^ static_cast<uint64_t>(height_);
        if (resourceSetId_ == 0u)
            resourceSetId_ = 1u;
        resourceGeneration_ = 1u;
        ready_ = true;
        publishDescriptor();
        Log(
            "OpenXRBridge: READY Vulkan-imported D3D11 NT frame resources "
            "adapterLuid=%08lx%08lx size=%ux%u vkFormat=%d slots=%u",
            static_cast<unsigned long>(
                static_cast<uint32_t>(adapterLuid_ >> 32u)),
            static_cast<unsigned long>(
                static_cast<uint32_t>(adapterLuid_)),
            width_,
            height_,
            static_cast<int>(vkFormat_),
            gtaiv_xr_bridge::SlotCount);
        return true;
    }

    bool createExactAdapterD3D11Device()
    {
        VkPhysicalDeviceIDProperties id {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &id;
        vk_.getPhysicalDeviceProperties2(vkPhysicalDevice_, &properties);
        if (!id.deviceLUIDValid)
        {
            Log("OpenXRBridge: Vulkan device has no valid Windows adapter LUID");
            return false;
        }

        LUID required {};
        static_assert(
            sizeof(required) == VK_LUID_SIZE,
            "Windows and Vulkan LUID sizes differ");
        std::memcpy(&required, id.deviceLUID, sizeof(required));
        adapterLuid_ = packLuid(required);

        ComPtr<IDXGIFactory1> factory;
        HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(result) || !factory)
        {
            Log(
                "OpenXRBridge: CreateDXGIFactory1 failed hr=0x%08lx",
                static_cast<unsigned long>(result));
            return false;
        }
        for (UINT index = 0u;; ++index)
        {
            ComPtr<IDXGIAdapter1> candidate;
            result = factory->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(result) || !candidate)
                return false;
            DXGI_ADAPTER_DESC1 description {};
            if (SUCCEEDED(candidate->GetDesc1(&description))
                && sameLuid(description.AdapterLuid, required))
            {
                dxgiAdapter_ = candidate;
                break;
            }
        }
        if (!dxgiAdapter_)
        {
            Log(
                "OpenXRBridge: no DXGI adapter matches DXVK Vulkan LUID "
                "%08lx%08lx",
                static_cast<unsigned long>(
                    static_cast<uint32_t>(adapterLuid_ >> 32u)),
                static_cast<unsigned long>(
                    static_cast<uint32_t>(adapterLuid_)));
            return false;
        }

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_11_0;
        result = D3D11CreateDevice(
            dxgiAdapter_.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,
            static_cast<UINT>(_countof(levels)),
            D3D11_SDK_VERSION,
            &d3d11Device_,
            &selected,
            &d3d11Context_);
        if (FAILED(result)
            || !d3d11Device_
            || !d3d11Context_
            || FAILED(d3d11Device_.As(&d3d11Device5_)))
        {
            Log(
                "OpenXRBridge: exact-adapter D3D11 fence device failed "
                "hr=0x%08lx",
                static_cast<unsigned long>(result));
            return false;
        }
        return true;
    }

    bool checkExternalCapabilities()
    {
        VkPhysicalDeviceExternalImageFormatInfo external {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        external.handleType = MemoryHandleType;
        VkPhysicalDeviceImageFormatInfo2 query {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        query.pNext = &external;
        query.format = vkFormat_;
        query.type = VK_IMAGE_TYPE_2D;
        query.tiling = VK_IMAGE_TILING_OPTIMAL;
        query.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        query.flags = 0u;

        VkExternalImageFormatProperties externalProperties {
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 properties {
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        properties.pNext = &externalProperties;
        const VkResult imageResult =
            vk_.getPhysicalDeviceImageFormatProperties2(
                vkPhysicalDevice_,
                &query,
                &properties);
        const VkExternalMemoryProperties& memory =
            externalProperties.externalMemoryProperties;
        if (imageResult != VK_SUCCESS
            || (memory.externalMemoryFeatures
                & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0u
            || (memory.compatibleHandleTypes & MemoryHandleType) == 0u)
        {
            Log(
                "OpenXRBridge: D3D11 texture import unsupported vk=%d "
                "features=0x%08lx compatible=0x%08lx",
                static_cast<int>(imageResult),
                static_cast<unsigned long>(memory.externalMemoryFeatures),
                static_cast<unsigned long>(memory.compatibleHandleTypes));
            return false;
        }

        VkPhysicalDeviceExternalSemaphoreInfo semaphoreQuery {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
        semaphoreQuery.handleType = FenceHandleType;
        VkExternalSemaphoreProperties semaphoreProperties {
            VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        vk_.getPhysicalDeviceExternalSemaphoreProperties(
            vkPhysicalDevice_,
            &semaphoreQuery,
            &semaphoreProperties);
        if ((semaphoreProperties.externalSemaphoreFeatures
             & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) == 0u
            || (semaphoreProperties.compatibleHandleTypes
                & FenceHandleType) == 0u)
        {
            Log(
                "OpenXRBridge: D3D11 fence import unsupported features=0x%08lx "
                "compatible=0x%08lx",
                static_cast<unsigned long>(
                    semaphoreProperties.externalSemaphoreFeatures),
                static_cast<unsigned long>(
                    semaphoreProperties.compatibleHandleTypes));
            return false;
        }
        return true;
    }

    bool createD3D11Fence(
        ComPtr<ID3D11Fence>& fence,
        HANDLE& handle,
        const char* label)
    {
        HRESULT result = d3d11Device5_->CreateFence(
            0u,
            D3D11_FENCE_FLAG_SHARED,
            IID_PPV_ARGS(&fence));
        if (FAILED(result) || !fence)
        {
            Log(
                "OpenXRBridge: CreateFence(%s) failed hr=0x%08lx",
                label,
                static_cast<unsigned long>(result));
            return false;
        }
        result = fence->CreateSharedHandle(
            nullptr,
            GENERIC_ALL,
            nullptr,
            &handle);
        if (FAILED(result) || !handle)
        {
            Log(
                "OpenXRBridge: CreateSharedHandle(%s fence) failed hr=0x%08lx",
                label,
                static_cast<unsigned long>(result));
            return false;
        }
        return true;
    }

    bool importD3D11Fence(HANDLE handle, VkSemaphore& semaphore)
    {
        VkSemaphoreTypeCreateInfo type {
            VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0u;
        VkSemaphoreCreateInfo create {
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        create.pNext = &type;
        VkResult result = vk_.createSemaphore(
            vkDevice_,
            &create,
            nullptr,
            &semaphore);
        if (result != VK_SUCCESS || semaphore == VK_NULL_HANDLE)
        {
            Log(
                "OpenXRBridge: vkCreateSemaphore(timeline) failed vk=%d",
                static_cast<int>(result));
            return false;
        }
        VkImportSemaphoreWin32HandleInfoKHR import {
            VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        import.semaphore = semaphore;
        import.flags = 0u;
        import.handleType = FenceHandleType;
        import.handle = handle;
        result = vk_.importSemaphoreWin32Handle(vkDevice_, &import);
        if (result != VK_SUCCESS)
        {
            Log(
                "OpenXRBridge: importing D3D11 fence into Vulkan failed vk=%d",
                static_cast<int>(result));
            return false;
        }
        return true;
    }

    bool createSharedFences()
    {
        return createD3D11Fence(
                   readyFence_,
                   readyFenceHandle_,
                   "ready")
            && createD3D11Fence(
                releaseFence_,
                releaseFenceHandle_,
                "release")
            && importD3D11Fence(
                readyFenceHandle_,
                readySemaphore_)
            && importD3D11Fence(
                releaseFenceHandle_,
                releaseSemaphore_);
    }

    bool chooseMemoryType(uint32_t allowedBits, uint32_t& resultIndex)
    {
        VkPhysicalDeviceMemoryProperties properties {};
        vk_.getPhysicalDeviceMemoryProperties(
            vkPhysicalDevice_,
            &properties);
        for (uint32_t index = 0u;
             index < properties.memoryTypeCount;
             ++index)
        {
            if ((allowedBits & (1u << index)) != 0u
                && (properties.memoryTypes[index].propertyFlags
                    & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u)
            {
                resultIndex = index;
                return true;
            }
        }
        for (uint32_t index = 0u;
             index < properties.memoryTypeCount;
             ++index)
        {
            if ((allowedBits & (1u << index)) != 0u)
            {
                resultIndex = index;
                return true;
            }
        }
        return false;
    }

    bool createSharedEye(SharedEye& eye)
    {
        D3D11_TEXTURE2D_DESC description {};
        description.Width = width_;
        description.Height = height_;
        description.MipLevels = 1u;
        description.ArraySize = 1u;
        description.Format = dxgiFormat_;
        description.SampleDesc.Count = 1u;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags =
            D3D11_RESOURCE_MISC_SHARED_NTHANDLE
            | D3D11_RESOURCE_MISC_SHARED;
        HRESULT hresult = d3d11Device_->CreateTexture2D(
            &description,
            nullptr,
            &eye.texture);
        if (FAILED(hresult) || !eye.texture)
        {
            Log(
                "OpenXRBridge: CreateTexture2D(NT stereo eye) failed "
                "hr=0x%08lx",
                static_cast<unsigned long>(hresult));
            return false;
        }
        ComPtr<IDXGIResource1> shareable;
        hresult = eye.texture.As(&shareable);
        if (FAILED(hresult)
            || !shareable
            || FAILED(shareable->CreateSharedHandle(
                nullptr,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                nullptr,
                &eye.sharedHandle))
            || !eye.sharedHandle)
        {
            Log(
                "OpenXRBridge: CreateSharedHandle(stereo eye) failed "
                "hr=0x%08lx",
                static_cast<unsigned long>(hresult));
            return false;
        }

        VkExternalMemoryImageCreateInfo external {
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = MemoryHandleType;
        VkImageCreateInfo image {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.pNext = &external;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = vkFormat_;
        image.extent = {width_, height_, 1u};
        image.mipLevels = 1u;
        image.arrayLayers = 1u;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vk_.createImage(
            vkDevice_,
            &image,
            nullptr,
            &eye.image);
        if (result != VK_SUCCESS || eye.image == VK_NULL_HANDLE)
        {
            Log(
                "OpenXRBridge: vkCreateImage(D3D11 import) failed vk=%d",
                static_cast<int>(result));
            return false;
        }

        VkMemoryRequirements requirements {};
        vk_.getImageMemoryRequirements(
            vkDevice_,
            eye.image,
            &requirements);
        VkMemoryWin32HandlePropertiesKHR handleProperties {
            VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        result = vk_.getMemoryWin32HandleProperties(
            vkDevice_,
            MemoryHandleType,
            eye.sharedHandle,
            &handleProperties);
        uint32_t memoryType = 0u;
        if (result != VK_SUCCESS
            || !chooseMemoryType(
                requirements.memoryTypeBits
                    & handleProperties.memoryTypeBits,
                memoryType))
        {
            Log(
                "OpenXRBridge: D3D11 eye memory properties rejected vk=%d "
                "imageBits=0x%08lx handleBits=0x%08lx",
                static_cast<int>(result),
                static_cast<unsigned long>(requirements.memoryTypeBits),
                static_cast<unsigned long>(handleProperties.memoryTypeBits));
            return false;
        }

        VkMemoryDedicatedAllocateInfo dedicated {
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = eye.image;
        VkImportMemoryWin32HandleInfoKHR import {
            VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        import.pNext = &dedicated;
        import.handleType = MemoryHandleType;
        import.handle = eye.sharedHandle;
        VkMemoryAllocateInfo allocate {
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &import;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memoryType;
        result = vk_.allocateMemory(
            vkDevice_,
            &allocate,
            nullptr,
            &eye.memory);
        if (result != VK_SUCCESS || eye.memory == VK_NULL_HANDLE)
        {
            Log(
                "OpenXRBridge: vkAllocateMemory(D3D11 import) failed vk=%d",
                static_cast<int>(result));
            return false;
        }
        result = vk_.bindImageMemory(
            vkDevice_,
            eye.image,
            eye.memory,
            0u);
        if (result != VK_SUCCESS)
        {
            Log(
                "OpenXRBridge: vkBindImageMemory(D3D11 import) failed vk=%d",
                static_cast<int>(result));
            return false;
        }
        return true;
    }

    bool createSharedSlots()
    {
        for (SharedSlot& slot : slots_)
        {
            for (SharedEye& eye : slot.eyes)
            {
                if (!createSharedEye(eye))
                    return false;
            }
            if (slot.eyes[0].sharedHandle == slot.eyes[1].sharedHandle
                || slot.eyes[0].image == slot.eyes[1].image)
            {
                Log("OpenXRBridge: created L/R resources unexpectedly alias");
                return false;
            }
        }
        return true;
    }

    bool createCommandPool()
    {
        VkCommandPoolCreateInfo pool {
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = vkQueueFamily_;
        VkResult result = vk_.createCommandPool(
            vkDevice_,
            &pool,
            nullptr,
            &commandPool_);
        if (result != VK_SUCCESS || commandPool_ == VK_NULL_HANDLE)
        {
            Log(
                "OpenXRBridge: vkCreateCommandPool failed vk=%d",
                static_cast<int>(result));
            return false;
        }

        std::array<VkCommandBuffer, gtaiv_xr_bridge::SlotCount> buffers {};
        VkCommandBufferAllocateInfo allocate {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = commandPool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount =
            static_cast<uint32_t>(buffers.size());
        result = vk_.allocateCommandBuffers(
            vkDevice_,
            &allocate,
            buffers.data());
        if (result != VK_SUCCESS)
        {
            Log(
                "OpenXRBridge: vkAllocateCommandBuffers failed vk=%d",
                static_cast<int>(result));
            return false;
        }
        for (uint32_t slot = 0u;
             slot < gtaiv_xr_bridge::SlotCount;
             ++slot)
        {
            slots_[slot].commandBuffer = buffers[slot];
        }
        return true;
    }

    bool createMapping()
    {
        mapping_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0u,
            sizeof(FrameBridge),
            gtaiv_xr_bridge::MappingName);
        if (!mapping_)
        {
            Log(
                "OpenXRBridge: CreateFileMapping(v5) failed win32=%lu",
                static_cast<unsigned long>(GetLastError()));
            return false;
        }
        shared_ = static_cast<FrameBridge*>(MapViewOfFile(
            mapping_,
            FILE_MAP_READ | FILE_MAP_WRITE,
            0u,
            0u,
            sizeof(FrameBridge)));
        if (!shared_)
        {
            Log(
                "OpenXRBridge: MapViewOfFile(v5) failed win32=%lu",
                static_cast<unsigned long>(GetLastError()));
            return false;
        }
        return true;
    }

    uint32_t findReusableSlot()
    {
        uint64_t completed = 0u;
        const VkResult result = vk_.getSemaphoreCounterValue(
            vkDevice_,
            releaseSemaphore_,
            &completed);
        if (result != VK_SUCCESS)
        {
            Log(
                "OpenXRBridge: release-fence counter failed vk=%d",
                static_cast<int>(result));
            return gtaiv_xr_bridge::SlotCount;
        }
        for (uint32_t offset = 0u;
             offset < gtaiv_xr_bridge::SlotCount;
             ++offset)
        {
            const uint32_t slot =
                (nextSlot_ + offset) % gtaiv_xr_bridge::SlotCount;
            if (slots_[slot].transactionId == 0u
                || completed >= slots_[slot].transactionId)
            {
                nextSlot_ = (slot + 1u) % gtaiv_xr_bridge::SlotCount;
                return slot;
            }
        }
        return gtaiv_xr_bridge::SlotCount;
    }

    bool recordCopy(
        SharedSlot& slot,
        const std::array<SourceEye, gtaiv_xr_bridge::EyeCount>& source,
        uint32_t copyEyeCount)
    {
        if (copyEyeCount == 0u
            || copyEyeCount > gtaiv_xr_bridge::EyeCount)
        {
            return false;
        }
        if (vk_.resetCommandBuffer(slot.commandBuffer, 0u) != VK_SUCCESS)
            return false;
        VkCommandBufferBeginInfo begin {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vk_.beginCommandBuffer(slot.commandBuffer, &begin) != VK_SUCCESS)
            return false;

        std::array<VkImageMemoryBarrier, gtaiv_xr_bridge::EyeCount>
            sourceToCopy {};
        std::array<VkImageMemoryBarrier, gtaiv_xr_bridge::EyeCount>
            destinationToCopy {};
        for (uint32_t eye = 0u;
             eye < copyEyeCount;
             ++eye)
        {
            sourceToCopy[eye] = {
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            sourceToCopy[eye].srcAccessMask =
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            sourceToCopy[eye].dstAccessMask =
                VK_ACCESS_TRANSFER_READ_BIT;
            sourceToCopy[eye].oldLayout = source[eye].layout;
            sourceToCopy[eye].newLayout =
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            sourceToCopy[eye].srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            sourceToCopy[eye].dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            sourceToCopy[eye].image = source[eye].image;
            sourceToCopy[eye].subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};

            destinationToCopy[eye] = {
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            destinationToCopy[eye].srcAccessMask = 0u;
            destinationToCopy[eye].dstAccessMask =
                VK_ACCESS_TRANSFER_WRITE_BIT;
            destinationToCopy[eye].oldLayout =
                slot.importedImageHasExternalContents
                ? VK_IMAGE_LAYOUT_GENERAL
                : VK_IMAGE_LAYOUT_UNDEFINED;
            destinationToCopy[eye].newLayout =
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            destinationToCopy[eye].srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_EXTERNAL;
            destinationToCopy[eye].dstQueueFamilyIndex =
                vkQueueFamily_;
            destinationToCopy[eye].image = slot.eyes[eye].image;
            destinationToCopy[eye].subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        }

        vk_.cmdPipelineBarrier(
            slot.commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u,
            0u,
            nullptr,
            0u,
            nullptr,
            copyEyeCount,
            sourceToCopy.data());
        vk_.cmdPipelineBarrier(
            slot.commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u,
            0u,
            nullptr,
            0u,
            nullptr,
            copyEyeCount,
            destinationToCopy.data());

        const VkImageCopy region {
            {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
            {0, 0, 0},
            {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
            {0, 0, 0},
            {width_, height_, 1u}};
        for (uint32_t eye = 0u;
             eye < copyEyeCount;
             ++eye)
        {
            vk_.cmdCopyImage(
                slot.commandBuffer,
                source[eye].image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                slot.eyes[eye].image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1u,
                &region);
        }

        std::array<VkImageMemoryBarrier, gtaiv_xr_bridge::EyeCount>
            sourceRestore {};
        std::array<VkImageMemoryBarrier, gtaiv_xr_bridge::EyeCount>
            destinationRelease {};
        for (uint32_t eye = 0u;
             eye < copyEyeCount;
             ++eye)
        {
            sourceRestore[eye] = {
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            sourceRestore[eye].srcAccessMask =
                VK_ACCESS_TRANSFER_READ_BIT;
            sourceRestore[eye].dstAccessMask =
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            sourceRestore[eye].oldLayout =
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            sourceRestore[eye].newLayout = source[eye].layout;
            sourceRestore[eye].srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            sourceRestore[eye].dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            sourceRestore[eye].image = source[eye].image;
            sourceRestore[eye].subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};

            destinationRelease[eye] = {
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            destinationRelease[eye].srcAccessMask =
                VK_ACCESS_TRANSFER_WRITE_BIT;
            destinationRelease[eye].dstAccessMask = 0u;
            destinationRelease[eye].oldLayout =
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            destinationRelease[eye].newLayout =
                VK_IMAGE_LAYOUT_GENERAL;
            destinationRelease[eye].srcQueueFamilyIndex =
                vkQueueFamily_;
            destinationRelease[eye].dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_EXTERNAL;
            destinationRelease[eye].image = slot.eyes[eye].image;
            destinationRelease[eye].subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        }
        vk_.cmdPipelineBarrier(
            slot.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0u,
            0u,
            nullptr,
            0u,
            nullptr,
            copyEyeCount,
            sourceRestore.data());
        vk_.cmdPipelineBarrier(
            slot.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0u,
            0u,
            nullptr,
            0u,
            nullptr,
            copyEyeCount,
            destinationRelease.data());
        return vk_.endCommandBuffer(slot.commandBuffer) == VK_SUCCESS;
    }

    bool submitCopy(
        uint32_t slotIndex,
        uint64_t transaction,
        const std::array<SourceEye, gtaiv_xr_bridge::EyeCount>& source,
        uint32_t copyEyeCount)
    {
        SharedSlot& slot = slots_[slotIndex];
        interop_->FlushRenderingCommands();
        interop_->LockSubmissionQueue();

        const bool recorded =
            recordCopy(slot, source, copyEyeCount);
        VkResult result = recorded ? VK_SUCCESS : VK_ERROR_UNKNOWN;
        if (recorded)
        {
            const uint64_t waitValue = slot.transactionId;
            const uint64_t signalValue = transaction;
            VkTimelineSemaphoreSubmitInfo timeline {
                VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timeline.waitSemaphoreValueCount =
                waitValue != 0u ? 1u : 0u;
            timeline.pWaitSemaphoreValues =
                waitValue != 0u ? &waitValue : nullptr;
            timeline.signalSemaphoreValueCount = 1u;
            timeline.pSignalSemaphoreValues = &signalValue;

            const VkPipelineStageFlags waitStage =
                VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.pNext = &timeline;
            submit.waitSemaphoreCount = waitValue != 0u ? 1u : 0u;
            submit.pWaitSemaphores =
                waitValue != 0u ? &releaseSemaphore_ : nullptr;
            submit.pWaitDstStageMask =
                waitValue != 0u ? &waitStage : nullptr;
            submit.commandBufferCount = 1u;
            submit.pCommandBuffers = &slot.commandBuffer;
            submit.signalSemaphoreCount = 1u;
            submit.pSignalSemaphores = &readySemaphore_;
            result = vk_.queueSubmit(vkQueue_, 1u, &submit, VK_NULL_HANDLE);
        }
        interop_->ReleaseSubmissionQueue();
        if (result != VK_SUCCESS)
        {
            Log(
                "OpenXRBridge: vkQueueSubmit(stereo pair) failed vk=%d",
                static_cast<int>(result));
            return false;
        }
        slot.importedImageHasExternalContents = true;
        return true;
    }

    FrameBridge descriptor() const
    {
        FrameBridge value {};
        value.magic = gtaiv_xr_bridge::Magic;
        value.version = gtaiv_xr_bridge::Version;
        value.structBytes = sizeof(FrameBridge);
        value.slotCount = gtaiv_xr_bridge::SlotCount;
        value.producerPid = GetCurrentProcessId();
        value.producerEpoch = producerEpoch_;
        value.resourceSetId = resourceSetId_;
        value.adapterLuid = adapterLuid_;
        value.width = width_;
        value.height = height_;
        value.format = cpuMailboxMode_
            ? cpuProtocolFormat_
            : protocolFormat(vkFormat_);
        value.flags = gtaiv_xr_bridge::Running;
        if (cpuMailboxMode_)
        {
            value.flags |= gtaiv_xr_bridge::CpuBgraMailbox;
            if (lastPresentationMode_
                == gtaiv_xr_bridge::PresentationMode::WorldStereo)
            {
                value.flags |= gtaiv_xr_bridge::Stereo
                    | gtaiv_xr_bridge::EyesDistinct;
            }
        }
        else
        {
            value.flags |= gtaiv_xr_bridge::GpuD3D11NtHandles
                | gtaiv_xr_bridge::Stereo
                | gtaiv_xr_bridge::EyesDistinct;
        }
        if (lastPair_.sameSimulationTick)
            value.flags |= gtaiv_xr_bridge::SameSimulationTick;
        else
            value.flags |= gtaiv_xr_bridge::TemporalStereo;
        if (lastPair_.poseStamped)
            value.flags |= gtaiv_xr_bridge::PoseStamped;
        if (lastPair_.verifiedWvpStereo)
            value.flags |= gtaiv_xr_bridge::VerifiedWvpStereo;
        if (lastPair_.verifiedDrawSceneStereo)
            value.flags |= gtaiv_xr_bridge::VerifiedDrawSceneStereo;
        if (lastPresentationMode_
            == gtaiv_xr_bridge::PresentationMode::WorldMono)
            value.flags |= gtaiv_xr_bridge::ImmersiveMono;
        value.currentSlot = currentSlot_;
        value.resourceGeneration = resourceGeneration_;
        value.transactionId = nextTransaction_;
        for (uint32_t eye = 0u;
             eye < gtaiv_xr_bridge::EyeCount;
             ++eye)
        {
            value.sourceFrameId[eye] =
                lastPair_.sourceFrameId[eye];
            value.poseSequence[eye] =
                lastPair_.poseSequence[eye];
            value.renderedDisplayTime[eye] =
                lastPair_.renderedDisplayTime[eye];
        }
        value.heartbeatTickMs = GetTickCount64();
        if (!cpuMailboxMode_)
        {
            value.readyFenceHandle = handleValue(readyFenceHandle_);
            value.releaseFenceHandle = handleValue(releaseFenceHandle_);
        }
        for (uint32_t slot = 0u;
             slot < gtaiv_xr_bridge::SlotCount;
             ++slot)
        {
            if (cpuMailboxMode_)
            {
                value.slots[slot].transactionId = cpuMailbox_
                    ? cpuMailbox_->slotTransactionId[slot]
                    : 0u;
            }
            else
            {
                for (uint32_t eye = 0u;
                     eye < gtaiv_xr_bridge::EyeCount;
                     ++eye)
                {
                    value.slots[slot].eyes[eye].textureHandle =
                        handleValue(slots_[slot].eyes[eye].sharedHandle);
                }
                value.slots[slot].transactionId =
                    slots_[slot].transactionId;
            }
        }
        value.mappingBytes = sizeof(FrameBridge);
        value.presentationMode = lastPresentationMode_;
        value.uiReasonFlags = lastUiReasonFlags_;
        value.uiEye = lastUiEye_;
        value.contentWidth = lastPair_.contentWidth;
        value.contentHeight = lastPair_.contentHeight;
        return value;
    }

    void commit(const FrameBridge& value)
    {
        if (!shared_)
            return;
        volatile LONG* sequence =
            reinterpret_cast<volatile LONG*>(
                &shared_->publicationSequence);
        LONG current = InterlockedCompareExchange(sequence, 0, 0);
        LONG writing = (current & 1) ? current + 2 : current + 1;
        InterlockedExchange(sequence, writing);
        MemoryBarrier();

        constexpr size_t prefixBytes =
            offsetof(FrameBridge, publicationSequence);
        constexpr size_t suffixOffset =
            offsetof(FrameBridge, publicationSequence)
            + sizeof(int32_t);
        std::memcpy(shared_, &value, prefixBytes);
        std::memcpy(
            reinterpret_cast<uint8_t*>(shared_) + suffixOffset,
            reinterpret_cast<const uint8_t*>(&value) + suffixOffset,
            sizeof(FrameBridge) - suffixOffset);
        MemoryBarrier();
        InterlockedExchange(sequence, writing + 1);
    }

    void publishDescriptor()
    {
        if (!ready_)
            return;
        commit(descriptor());
        lastHeartbeatPublishTick_ = GetTickCount64();
    }

    void publishHeartbeat()
    {
        if (!ready_)
            return;
        const uint64_t now = GetTickCount64();
        if (now - lastHeartbeatPublishTick_ >= 500u)
            publishDescriptor();
    }

    void disable(const char* reason)
    {
        if (permanentlyDisabled_)
            return;
        permanentlyDisabled_ = true;
        Log(
            "OpenXRBridge: DISABLED for this process - %s; no retry, "
            "no compositor fallback",
            reason);
        if (shared_)
        {
            FrameBridge stopped = descriptor();
            stopped.flags = 0u;
            stopped.heartbeatTickMs = GetTickCount64();
            commit(stopped);
        }
    }

    void logRateLimited(const char* text, uint64_t& lastTick)
    {
        const uint64_t now = GetTickCount64();
        if (lastTick == 0u || now - lastTick >= 5000u)
        {
            Log("%s", text);
            lastTick = now;
        }
    }

    bool ready_ = false;
    bool permanentlyDisabled_ = false;
    bool cpuMailboxMode_ = false;
    ComPtr<IDirect3DDevice9> gameDevice_;
    ComPtr<ID3D9VkInteropDevice> interop_;
    VulkanFunctions vk_;
    VkInstance vkInstance_ = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhysicalDevice_ = VK_NULL_HANDLE;
    VkDevice vkDevice_ = VK_NULL_HANDLE;
    VkQueue vkQueue_ = VK_NULL_HANDLE;
    uint32_t vkQueueIndex_ = 0u;
    uint32_t vkQueueFamily_ = 0u;
    VkFormat vkFormat_ = VK_FORMAT_UNDEFINED;

    ComPtr<IDXGIAdapter1> dxgiAdapter_;
    ComPtr<ID3D11Device> d3d11Device_;
    ComPtr<ID3D11Device5> d3d11Device5_;
    ComPtr<ID3D11DeviceContext> d3d11Context_;
    DXGI_FORMAT dxgiFormat_ = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Fence> readyFence_;
    ComPtr<ID3D11Fence> releaseFence_;
    HANDLE readyFenceHandle_ = nullptr;
    HANDLE releaseFenceHandle_ = nullptr;
    VkSemaphore readySemaphore_ = VK_NULL_HANDLE;
    VkSemaphore releaseSemaphore_ = VK_NULL_HANDLE;

    std::array<SharedSlot, gtaiv_xr_bridge::SlotCount> slots_;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    HANDLE mapping_ = nullptr;
    FrameBridge* shared_ = nullptr;
    HANDLE cpuFrameMapping_ = nullptr;
    gtaiv_xr_bridge::CpuFrameMailbox* cpuMailbox_ = nullptr;
    std::array<
        ComPtr<IDirect3DSurface9>,
        gtaiv_xr_bridge::EyeCount> cpuReadbacks_;
    D3DFORMAT cpuD3dFormat_ = D3DFMT_UNKNOWN;
    PixelFormat cpuProtocolFormat_ = PixelFormat::Unknown;

    OpenXrStereoPair lastPair_ {};
    uint64_t producerEpoch_ = 0u;
    uint64_t resourceSetId_ = 0u;
    uint64_t adapterLuid_ = 0u;
    uint64_t nextTransaction_ = 0u;
    uint64_t lastPairId_ = 0u;
    uint64_t lastHeartbeatPublishTick_ = 0u;
    uint64_t lastPairWaitLogTick_ = 0u;
    uint64_t lastRouteWaitLogTick_ = 0u;
    uint64_t lastBusyLogTick_ = 0u;
    uint64_t lastCpuFailureLogTick_ = 0u;
    uint64_t cpuCaptureCount_ = 0u;
    uint32_t resourceGeneration_ = 0u;
    uint32_t currentSlot_ = 0u;
    uint32_t nextSlot_ = 0u;
    uint32_t width_ = 0u;
    uint32_t height_ = 0u;
    gtaiv_xr_bridge::PresentationMode lastPresentationMode_ =
        gtaiv_xr_bridge::PresentationMode::Unknown;
    gtaiv_xr_bridge::PresentationMode lastLoggedCpuPresentation_ =
        gtaiv_xr_bridge::PresentationMode::Unknown;
    uint32_t lastUiReasonFlags_ = gtaiv_xr_bridge::UiReasonNone;
    uint32_t lastUiEye_ = 0u;
};

// Process-lifetime allocation deliberately avoids Vulkan/D3D teardown from
// static destructors while Windows holds the loader lock.
OpenXrFrameProducer* g_producer = new OpenXrFrameProducer();
uint32_t g_openXrValidFrames = 0u;
bool g_openXrGameplayArmed = false;
bool g_openXrArmAttempted = false;
bool g_openXrPoseLostLogged = false;
bool g_openXrOpenVrLoadedLogged = false;
gtaiv_xr_bridge::PoseBridge g_openXrPoseForNextFrame {};
bool g_openXrHavePoseForNextFrame = false;
}

VrBackend GetVrBackend()
{
    static const VrBackend backend = [] {
        std::ifstream input(moduleDirectory() + "gtaiv_dxvk_vr.backend");
        std::string value;
        std::getline(input, value);
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
        value.erase(
            std::remove_if(
                value.begin(),
                value.end(),
                [](unsigned char character) {
                    return std::isspace(character) != 0;
                }),
            value.end());

        VrBackend selected = VrBackend::Off;
        if (value == "openxr")
            selected = VrBackend::OpenXr;
        else if (value == "openvr")
            selected = VrBackend::OpenVr;
        Log(
            "Backend: %s (file=%s)",
            selected == VrBackend::OpenXr
                ? "OpenXR sidecar"
                : selected == VrBackend::OpenVr
                    ? "OpenVR/SteamVR"
                    : "Off (flat; no compositor)",
            value.empty() ? "missing/default" : value.c_str());
        return selected;
    }();
    return backend;
}

bool IsOpenXrBridgeRequested()
{
    return GetVrBackend() == VrBackend::OpenXr;
}

bool IsOpenVrBridgeRequested()
{
    return GetVrBackend() == VrBackend::OpenVr;
}

void PublishOpenXrFrame(IDirect3DDevice9* device)
{
    const auto openVrLoaded = [] {
        if (!GetModuleHandleW(L"openvr_api.dll"))
            return false;
        if (!g_openXrOpenVrLoadedLogged)
        {
            g_openXrOpenVrLoadedLogged = true;
            Log(
                "OpenXRBridge: FAIL-CLOSED - openvr_api.dll was loaded while "
                "backend=openxr; camera and frame publication disabled");
        }
        g_openXrGameplayArmed = false;
        g_openXrArmAttempted = true;
        g_openXrHavePoseForNextFrame = false;
        StereoSetOpenXrRenderPose(nullptr);
        SetCamMatrixGameplayActive(false);
        return true;
    };
    if (openVrLoaded())
        return;

    gtaiv_xr_bridge::PoseBridge pose {};
    if (!GetLatestOpenXrPoseBridge(&pose))
    {
        InvalidateHmdPose();
        g_openXrHavePoseForNextFrame = false;
        StereoSetOpenXrRenderPose(nullptr);
        SetCamMatrixGameplayActive(false);
        g_openXrValidFrames = 0u;
        if (!g_openXrPoseLostLogged)
        {
            Log(
                "OpenXRBridge: fresh host pose unavailable; camera/input neutral "
                "and frame heartbeat paused");
            g_openXrPoseLostLogged = true;
        }
        if (g_producer)
            g_producer->heartbeat();
        return;
    }
    g_openXrPoseLostLogged = false;
    StereoSetOpenXrRenderPose(
        g_openXrHavePoseForNextFrame
            ? &g_openXrPoseForNextFrame
            : nullptr);
    g_openXrPoseForNextFrame = pose;
    g_openXrHavePoseForNextFrame = true;
    UpdateHmdPoseFromOpenXr(pose);

    if (!g_openXrGameplayArmed)
    {
        if (++g_openXrValidFrames < 360u)
            return;
        if (g_openXrArmAttempted)
            return;
        g_openXrArmAttempted = true;
        ReloadStereoMode();
        const StereoMode directMode = GetStereoMode();
        if (!IsOpenXrDirectMode(directMode))
        {
            Log(
                "OpenXRBridge: direct world presentation requires stereo mode "
                "55 (immersive mono), 56 (guarded DrawScene stereo), or 54 "
                "(experimental WVP); current mode=%d, so no GTA "
                "camera, controller, or frame hooks were armed",
                static_cast<int>(directMode));
            return;
        }
        if (!InstallUiStateProbe())
        {
            Log(
                "OpenXRBridge: menu/loading/phone state probes did not "
                "arm; presentation remains fail-closed");
            return;
        }
        const bool cameraReady = InstallCamMatrixHooks();
        if (!cameraReady)
        {
            Log(
                "OpenXRBridge: GTA camera hooks did not arm; stereo transport "
                "remains fail-closed");
            return;
        }
        SetCamMatrixGameplayActive(true);
        const bool stereoReady = InstallStereoRenderHooks();
        if (!stereoReady)
        {
            SetCamMatrixGameplayActive(false);
            Log(
                "OpenXRBridge: stereo capture hooks did not arm; no frame "
                "transaction will be published");
            return;
        }
        if (!InstallOpenXrControllerHooks())
        {
            SetCamMatrixGameplayActive(false);
            Log(
                "OpenXRBridge: Touch/XInput hooks did not arm; gameplay "
                "remains fail-closed");
            return;
        }
        UpdatePedHeadHide();
        g_openXrGameplayArmed = true;
        Log(
            "OpenXRBridge: GTA camera/frame capture armed in mode %d after "
            "360 fresh host samples; display FOV comes from OpenXR and "
            "OpenVR remains uninitialized",
            static_cast<int>(directMode));
        // Hooks armed at this EndScene can only affect the next GTA frame.
        // Do not stamp the already-rendered native frame as head-owned.
        return;
    }
    else
    {
        SetCamMatrixGameplayActive(true);
    }

    StereoRenderOnDevice(device);
    if (openVrLoaded())
        return;
    UpdateGameFovFromDevice(device);
    PollCamHotkeys();
    PollIpdScaleHotkey();
    PollWorldScaleHotkey();
    PollStereoScaleHotkey();
    PumpOpenXrControllerBridge();
    if (g_producer)
        g_producer->publish(device);
    // Keep the established GTA-side locomotion/right-stick behavior identical
    // across compositors. OpenXR only supplies the cached HMD pose and XInput
    // state; vr_move.cpp remains the single owner of gameplay movement policy.
    UpdateVrMoveAndStick();
}

void ShutdownOpenXrBridge()
{
    g_openXrHavePoseForNextFrame = false;
    StereoSetOpenXrRenderPose(nullptr);
    if (g_producer)
        g_producer->announceStopped();
}
}
