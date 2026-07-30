#include "game_bridge.h"

#include "../bridge/gtaiv_xr_frame_bridge.h"

#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <utility>
#include <vector>

namespace gtaiv_xr_host
{
namespace
{
using Microsoft::WRL::ComPtr;
using gtaiv_xr_bridge::FrameBridge;

uint64_t packLuid(const LUID& luid)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32u)
        | static_cast<uint64_t>(luid.LowPart);
}

DXGI_FORMAT dxgiFormat(gtaiv_xr_bridge::PixelFormat format)
{
    switch (format)
    {
        case gtaiv_xr_bridge::PixelFormat::B8G8R8A8Unorm:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case gtaiv_xr_bridge::PixelFormat::B8G8R8X8Unorm:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case gtaiv_xr_bridge::PixelFormat::R8G8B8A8Unorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT privateStorageFormat(gtaiv_xr_bridge::PixelFormat format)
{
    switch (format)
    {
        case gtaiv_xr_bridge::PixelFormat::B8G8R8A8Unorm:
        case gtaiv_xr_bridge::PixelFormat::B8G8R8X8Unorm:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case gtaiv_xr_bridge::PixelFormat::R8G8B8A8Unorm:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT privateViewFormat(gtaiv_xr_bridge::PixelFormat format)
{
    switch (format)
    {
        case gtaiv_xr_bridge::PixelFormat::B8G8R8A8Unorm:
        case gtaiv_xr_bridge::PixelFormat::B8G8R8X8Unorm:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case gtaiv_xr_bridge::PixelFormat::R8G8B8A8Unorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

bool colorPathSelfTest()
{
    const D3D_FEATURE_LEVEL requested[] = {
        D3D_FEATURE_LEVEL_11_0
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            requested,
            1u,
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            &context))
        || !device
        || !context)
    {
        return false;
    }

    const uint8_t pixels[4] = { 0x40u, 0x80u, 0xc0u, 0xffu };
    D3D11_SUBRESOURCE_DATA initial {};
    initial.pSysMem = pixels;
    initial.SysMemPitch = 4u;
    D3D11_TEXTURE2D_DESC sourceDescription {};
    sourceDescription.Width = 1u;
    sourceDescription.Height = 1u;
    sourceDescription.MipLevels = 1u;
    sourceDescription.ArraySize = 1u;
    sourceDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sourceDescription.SampleDesc.Count = 1u;
    sourceDescription.Usage = D3D11_USAGE_DEFAULT;
    sourceDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(device->CreateTexture2D(
            &sourceDescription,
            &initial,
            &source))
        || !source)
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC privateDescription = sourceDescription;
    privateDescription.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    ComPtr<ID3D11Texture2D> privateTexture;
    if (FAILED(device->CreateTexture2D(
            &privateDescription,
            nullptr,
            &privateTexture))
        || !privateTexture)
    {
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription {};
    viewDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.MipLevels = 1u;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateShaderResourceView(
            privateTexture.Get(),
            &viewDescription,
            &view))
        || !view)
    {
        return false;
    }

    context->CopyResource(privateTexture.Get(), source.Get());
    D3D11_TEXTURE2D_DESC stagingDescription = privateDescription;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0u;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(
            &stagingDescription,
            nullptr,
            &staging))
        || !staging)
    {
        return false;
    }
    context->CopyResource(staging.Get(), privateTexture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(
            staging.Get(),
            0u,
            D3D11_MAP_READ,
            0u,
            &mapped))
        || !mapped.pData)
    {
        return false;
    }
    const auto* copied = static_cast<const uint8_t*>(mapped.pData);
    const bool bytesMatch =
        copied[0] == pixels[0]
        && copied[1] == pixels[1]
        && copied[2] == pixels[2]
        && copied[3] == pixels[3];
    context->Unmap(staging.Get(), 0u);
    return bytesMatch && device->GetDeviceRemovedReason() == S_OK;
}

bool rawHandleValid(uint64_t value)
{
    return value != 0u
        && value <= static_cast<uint64_t>(
            (std::numeric_limits<uintptr_t>::max)())
        && static_cast<uintptr_t>(value)
            != reinterpret_cast<uintptr_t>(INVALID_HANDLE_VALUE);
}

bool stableSnapshot(const FrameBridge* shared, FrameBridge& output)
{
    if (!shared)
        return false;
    const int32_t first = shared->publicationSequence;
    if (first == 0 || (first & 1) != 0)
        return false;
    MemoryBarrier();
    std::memcpy(&output, shared, sizeof(output));
    MemoryBarrier();
    const int32_t second = shared->publicationSequence;
    return first == second && (second & 1) == 0;
}

struct TransactionQuality
{
    bool accepted = false;
    bool sameTick = false;
    bool temporalStereo = false;
    bool parentDualStereo = false;
    bool firstPersonCamera = false;
    bool nativeHeadHidden = false;
    bool pixelDistinct = false;
    bool uiQuad = false;
    bool immersiveMono = false;
    const char* rejection = nullptr;
};

TransactionQuality assessTransactionQuality(
    const FrameBridge& value,
    bool allowTemporalStereo)
{
    TransactionQuality result {};
    const bool sameTickFlag =
        (value.flags & gtaiv_xr_bridge::SameSimulationTick) != 0u;
    const bool temporalStereoFlag =
        (value.flags & gtaiv_xr_bridge::TemporalStereo) != 0u;
    const bool poseStampedFlag =
        (value.flags & gtaiv_xr_bridge::PoseStamped) != 0u;
    result.sameTick =
        sameTickFlag
        && value.sourceFrameId[0] != 0u
        && value.sourceFrameId[0] == value.sourceFrameId[1]
        && value.poseSequence[0] != 0u
        && value.poseSequence[0] == value.poseSequence[1]
        && value.renderedDisplayTime[0] != 0
        && value.renderedDisplayTime[0]
            == value.renderedDisplayTime[1];

    const bool worldStereo =
        value.presentationMode
            == gtaiv_xr_bridge::PresentationMode::WorldStereo;
    result.immersiveMono =
        value.presentationMode
            == gtaiv_xr_bridge::PresentationMode::WorldMono;
    const bool verifiedWvpStereo =
        (value.flags & gtaiv_xr_bridge::VerifiedWvpStereo) != 0u;
    const bool verifiedDrawSceneStereo =
        (value.flags & gtaiv_xr_bridge::VerifiedDrawSceneStereo) != 0u;
    const bool verifiedParentDualStereo =
        (value.flags
            & gtaiv_xr_bridge::VerifiedParentDualStereo) != 0u;
    const bool firstPersonCamera =
        (value.flags & gtaiv_xr_bridge::FirstPersonCamera) != 0u;
    const bool nativeHeadHidden =
        (value.flags & gtaiv_xr_bridge::NativeHeadHidden) != 0u;
    const bool distinctEyes =
        (value.flags
            & (gtaiv_xr_bridge::Stereo
                | gtaiv_xr_bridge::EyesDistinct))
        == (gtaiv_xr_bridge::Stereo
            | gtaiv_xr_bridge::EyesDistinct);
    result.parentDualStereo =
        worldStereo
        && verifiedParentDualStereo
        && distinctEyes
        && result.sameTick
        && !temporalStereoFlag;
    result.firstPersonCamera = firstPersonCamera;
    result.nativeHeadHidden = nativeHeadHidden;
    result.pixelDistinct = worldStereo && distinctEyes;
    const bool immersiveMonoFlag =
        (value.flags & gtaiv_xr_bridge::ImmersiveMono) != 0u;
    // A temporal pair is an ordered L-then-R capture, not merely a world
    // transaction that omitted SameSimulationTick. Require every render stamp
    // to advance so accidental stale/duplicated eyes cannot enter the opt-in
    // route.
    result.temporalStereo =
        temporalStereoFlag
        && !sameTickFlag
        && value.sourceFrameId[0] != 0u
        && value.sourceFrameId[0] < value.sourceFrameId[1]
        && value.poseSequence[0] != 0u
        && value.poseSequence[0] < value.poseSequence[1]
        && value.renderedDisplayTime[0] != 0
        && value.renderedDisplayTime[0]
            < value.renderedDisplayTime[1];
    result.uiQuad =
        value.presentationMode
            == gtaiv_xr_bridge::PresentationMode::UiQuad;
    constexpr uint32_t KnownUiReasons =
        gtaiv_xr_bridge::UiReasonPauseOrMap
        | gtaiv_xr_bridge::UiReasonLoading
        | gtaiv_xr_bridge::UiReasonPhone;
    const bool reasonsValid =
        (value.uiReasonFlags & ~KnownUiReasons) == 0u;
    if ((!worldStereo && !result.immersiveMono && !result.uiQuad)
        || !poseStampedFlag
        || value.uiEye >= gtaiv_xr_bridge::EyeCount
        || value.contentWidth == 0u
        || value.contentHeight == 0u
        || value.contentWidth > 32768u
        || value.contentHeight > 32768u
        || !reasonsValid
        || ((worldStereo || result.immersiveMono)
            && value.uiReasonFlags != gtaiv_xr_bridge::UiReasonNone)
        || (result.uiQuad
            && value.uiReasonFlags == gtaiv_xr_bridge::UiReasonNone)
        || (worldStereo && immersiveMonoFlag)
        || (worldStereo && !distinctEyes)
        || (sameTickFlag && temporalStereoFlag)
        || ((verifiedParentDualStereo
                || firstPersonCamera
                || nativeHeadHidden)
            && !worldStereo)
        || ((firstPersonCamera || nativeHeadHidden)
            && !verifiedParentDualStereo)
        || (result.immersiveMono
            && (!immersiveMonoFlag
                || verifiedWvpStereo
                || verifiedDrawSceneStereo
                || verifiedParentDualStereo
                || firstPersonCamera
                || nativeHeadHidden
                || temporalStereoFlag))
        || (result.uiQuad
            && (immersiveMonoFlag
                || verifiedWvpStereo
                || verifiedDrawSceneStereo
                || verifiedParentDualStereo
                || firstPersonCamera
                || nativeHeadHidden)))
    {
        result.rejection = "invalid presentation mode";
        return result;
    }
    if (worldStereo && temporalStereoFlag)
    {
        if (!allowTemporalStereo)
        {
            result.rejection = "temporal stereo was not explicitly enabled";
            return result;
        }
        if (!result.temporalStereo)
        {
            result.rejection = "temporal stereo stamps are not ordered";
            return result;
        }
        if (verifiedWvpStereo
            || verifiedDrawSceneStereo
            || verifiedParentDualStereo)
        {
            result.rejection =
                "temporal stereo claimed a same-frame proof";
            return result;
        }
    }
    else if (worldStereo
             && (!result.sameTick
                 || (!verifiedWvpStereo
                     && !verifiedDrawSceneStereo
                     && !verifiedParentDualStereo)))
    {
        result.rejection = "missing verified same-frame stereo proof";
        return result;
    }
    if (verifiedParentDualStereo && !result.parentDualStereo)
    {
        result.rejection =
            "parent-dual stereo lacks same-tick distinct-eye proof";
        return result;
    }
    if (result.immersiveMono && !result.sameTick)
    {
        result.rejection = "immersive mono is not one simulation tick/pose";
        return result;
    }
    if (value.poseSequence[0] == 0u
        || value.poseSequence[1] == 0u
        || value.renderedDisplayTime[value.uiEye] == 0
        || value.sourceFrameId[value.uiEye] == 0u)
    {
        result.rejection = "missing pose/source stamp";
        return result;
    }
    result.accepted = true;
    return result;
}
}

struct GameBridge::Implementation
{
    using SharedTextureSet = std::array<
        std::array<ComPtr<ID3D11Texture2D>, gtaiv_xr_bridge::EyeCount>,
        gtaiv_xr_bridge::SlotCount>;
    struct PrivateSlot
    {
        std::array<
            ComPtr<ID3D11Texture2D>,
            gtaiv_xr_bridge::EyeCount> ingestTextures;
        std::array<
            ComPtr<ID3D11Texture2D>,
            gtaiv_xr_bridge::EyeCount> renderTextures;
        std::array<
            ComPtr<ID3D11ShaderResourceView>,
            gtaiv_xr_bridge::EyeCount> views;
        uint64_t transactionId = 0u;
    };

    explicit Implementation(LogFunction function, bool allowTemporal)
        : logger(std::move(function))
        , allowTemporalStereo(allowTemporal)
    {
    }

    ~Implementation()
    {
        resetAll();
    }

    void log(const std::string& value) const
    {
        if (logger)
            logger(value);
    }

    void logRateLimited(
        const std::string& message,
        uint64_t& lastTick,
        uint64_t interval = 5000u)
    {
        const uint64_t now = GetTickCount64();
        if (lastTick == 0u || now - lastTick >= interval)
        {
            log(message);
            lastTick = now;
        }
    }

    bool initialize(
        ID3D11Device* sourceDevice,
        ID3D11DeviceContext* sourceContext)
    {
        if (!sourceDevice || !sourceContext)
            return false;
        device = sourceDevice;
        context = sourceContext;
        if (FAILED(device.As(&device5))
            || FAILED(context.As(&context4))
            || !acquireAdapterLuid()
            || !createIngestDevice())
        {
            log("GameBridge: isolated D3D11 ingest queue unavailable");
            resetAll();
            return false;
        }
        log(
            "GameBridge: isolated GPU ingest queue READY "
            "(GTA waits/copies cannot block OpenXR rendering)");
        log(
            allowTemporalStereo
                ? "GameBridge: waiting for GTA stereo; TEMPORAL diagnostic explicitly allowed"
                : "GameBridge: waiting for strict same-tick GTA stereo");
        return true;
    }

    bool acquireAdapterLuid()
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description {};
        if (FAILED(device.As(&dxgiDevice))
            || FAILED(dxgiDevice->GetAdapter(&adapter))
            || FAILED(adapter->GetDesc(&description)))
        {
            return false;
        }
        adapterLuid = packLuid(description.AdapterLuid);
        return adapterLuid != 0u;
    }

    bool createIngestDevice()
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description {};
        if (FAILED(device.As(&dxgiDevice))
            || FAILED(dxgiDevice->GetAdapter(&adapter))
            || FAILED(adapter->GetDesc(&description))
            || packLuid(description.AdapterLuid) != adapterLuid)
        {
            return false;
        }

        const D3D_FEATURE_LEVEL requested = device->GetFeatureLevel();
        D3D_FEATURE_LEVEL created = D3D_FEATURE_LEVEL_9_1;
        const HRESULT result = D3D11CreateDevice(
            adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            &requested,
            1u,
            D3D11_SDK_VERSION,
            &ingestDevice,
            &created,
            &ingestContext);
        if (FAILED(result)
            || !ingestDevice
            || !ingestContext
            || created != requested
            || FAILED(ingestDevice.As(&ingestDevice5))
            || FAILED(ingestContext.As(&ingestContext4)))
        {
            return false;
        }
        return true;
    }

    void releaseResources()
    {
        for (ComPtr<ID3D11ShaderResourceView>& view : cpuViews)
            view.Reset();
        for (ComPtr<ID3D11Texture2D>& texture : cpuTextures)
            texture.Reset();
        for (std::vector<uint8_t>& pixels : cpuPixels)
            pixels.clear();
        if (cpuMapping)
        {
            UnmapViewOfFile(cpuMapping);
            cpuMapping = nullptr;
        }
        if (cpuMappingHandle)
        {
            CloseHandle(cpuMappingHandle);
            cpuMappingHandle = nullptr;
        }
        cpuMailboxMode = false;
        for (PrivateSlot& slot : privateSlots)
        {
            for (ComPtr<ID3D11ShaderResourceView>& view : slot.views)
                view.Reset();
            for (ComPtr<ID3D11Texture2D>& texture : slot.renderTextures)
                texture.Reset();
            for (ComPtr<ID3D11Texture2D>& texture : slot.ingestTextures)
                texture.Reset();
            slot.transactionId = 0u;
        }
        for (auto& slot : sharedTextures)
        {
            for (ComPtr<ID3D11Texture2D>& texture : slot)
                texture.Reset();
        }
        renderCopyReadyFence.Reset();
        ingestCopyReadyFence.Reset();
        ingestPrivateReleaseFence.Reset();
        renderPrivateReleaseFence.Reset();
        releaseFence.Reset();
        readyFence.Reset();
        if (producerProcess)
        {
            CloseHandle(producerProcess);
            producerProcess = nullptr;
        }
        connectedPid = 0u;
        connectedEpoch = 0u;
        connectedResourceSet = 0u;
        connectedGeneration = 0u;
        lastEnqueuedTransaction = 0u;
        lastCpuMailboxTransaction = 0u;
        acquiredFrameCount = 0u;
        lastLoggedCpuPresentationMode =
            gtaiv_xr_bridge::PresentationMode::Unknown;
        pendingPrivateSlot = gtaiv_xr_bridge::SlotCount;
        currentPrivateSlot = gtaiv_xr_bridge::SlotCount;
        pendingFrame = {};
        pendingEnqueueTick = 0u;
        current = {};
    }

    bool createCpuTextures(const FrameBridge& value)
    {
        const DXGI_FORMAT storage = privateStorageFormat(value.format);
        const DXGI_FORMAT view = privateViewFormat(value.format);
        if (storage == DXGI_FORMAT_UNKNOWN || view == DXGI_FORMAT_UNKNOWN)
            return false;

        D3D11_TEXTURE2D_DESC description {};
        description.Width = value.width;
        description.Height = value.height;
        description.MipLevels = 1u;
        description.ArraySize = 1u;
        description.Format = storage;
        description.SampleDesc.Count = 1u;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription {};
        viewDescription.Format = view;
        viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDescription.Texture2D.MostDetailedMip = 0u;
        viewDescription.Texture2D.MipLevels = 1u;
        for (uint32_t eye = 0u;
             eye < gtaiv_xr_bridge::EyeCount;
             ++eye)
        {
            if (FAILED(device->CreateTexture2D(
                    &description,
                    nullptr,
                    &cpuTextures[eye]))
                || !cpuTextures[eye])
            {
                return false;
            }
            if (FAILED(device->CreateShaderResourceView(
                    cpuTextures[eye].Get(),
                    &viewDescription,
                    &cpuViews[eye]))
                || !cpuViews[eye])
            {
                return false;
            }
        }
        return cpuTextures[0].Get() != cpuTextures[1].Get()
            && cpuViews[0].Get() != cpuViews[1].Get();
    }

    bool openCpuMailbox(const FrameBridge& value)
    {
        releaseResources();
        const uint32_t forbiddenGpuFlags =
            gtaiv_xr_bridge::GpuD3D11NtHandles;
        if (value.width == 0u
            || value.height == 0u
            || value.width > gtaiv_xr_bridge::CpuFrameMaxWidth
            || value.height > gtaiv_xr_bridge::CpuFrameMaxHeight
            || dxgiFormat(value.format) == DXGI_FORMAT_UNKNOWN
            || (value.flags & gtaiv_xr_bridge::CpuBgraMailbox) == 0u
            || (value.flags & forbiddenGpuFlags) != 0u)
        {
            logRateLimited(
                "GameBridge: CPU mailbox descriptor rejected",
                lastOpenFailureLogTick);
            return false;
        }

        constexpr DWORD access =
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
        producerProcess = OpenProcess(access, FALSE, value.producerPid);
        if (!producerProcess || !producerActive(value.producerPid))
        {
            logRateLimited(
                "GameBridge: CPU mailbox producer process is unavailable",
                lastOpenFailureLogTick);
            releaseResources();
            return false;
        }

        cpuMappingHandle = OpenFileMappingW(
            FILE_MAP_READ,
            FALSE,
            gtaiv_xr_bridge::CpuFrameMappingName);
        if (!cpuMappingHandle)
        {
            logRateLimited(
                "GameBridge: CPU frame mailbox is not available yet",
                lastOpenFailureLogTick);
            releaseResources();
            return false;
        }
        cpuMapping = static_cast<const gtaiv_xr_bridge::CpuFrameMailbox*>(
            MapViewOfFile(
                cpuMappingHandle,
                FILE_MAP_READ,
                0u,
                0u,
                static_cast<SIZE_T>(gtaiv_xr_bridge::CpuFrameMappingBytes)));
        if (!cpuMapping
            || cpuMapping->magic != gtaiv_xr_bridge::CpuFrameMagic
            || cpuMapping->version != gtaiv_xr_bridge::CpuFrameVersion
            || cpuMapping->headerBytes
                != sizeof(gtaiv_xr_bridge::CpuFrameMailbox)
            || cpuMapping->slotCount != gtaiv_xr_bridge::SlotCount
            || cpuMapping->maxWidth != gtaiv_xr_bridge::CpuFrameMaxWidth
            || cpuMapping->maxHeight != gtaiv_xr_bridge::CpuFrameMaxHeight
            || cpuMapping->bytesPerPixel
                != gtaiv_xr_bridge::CpuFrameBytesPerPixel
            || cpuMapping->eyeCount
                != gtaiv_xr_bridge::CpuFrameEyeCount
            || cpuMapping->mappingBytes
                != gtaiv_xr_bridge::CpuFrameMappingBytes
            || !createCpuTextures(value))
        {
            logRateLimited(
                "GameBridge: CPU frame mailbox layout or texture setup failed",
                lastOpenFailureLogTick);
            releaseResources();
            return false;
        }

        connectedPid = value.producerPid;
        connectedEpoch = value.producerEpoch;
        connectedResourceSet = value.resourceSetId;
        connectedGeneration = value.resourceGeneration;
        cpuMailboxMode = true;
        std::ostringstream message;
        message << "GameBridge: CONNECTED FNVVR-style CPU mailbox pid="
                << connectedPid
                << " source=" << value.width << 'x' << value.height
                << " eyes=" << gtaiv_xr_bridge::CpuFrameEyeCount
                << " (no GTA/DXVK release fence; sRGB decode SRV)";
        log(message.str());
        return true;
    }

    void releaseMapping()
    {
        if (shared)
        {
            UnmapViewOfFile(shared);
            shared = nullptr;
        }
        if (mapping)
        {
            CloseHandle(mapping);
            mapping = nullptr;
        }
    }

    void resetAll()
    {
        releaseResources();
        releaseMapping();
        context4.Reset();
        context.Reset();
        device5.Reset();
        device.Reset();
        ingestContext4.Reset();
        ingestContext.Reset();
        ingestDevice5.Reset();
        ingestDevice.Reset();
    }

    bool ensureMapping()
    {
        if (mapping && shared)
            return true;
        releaseMapping();
        mapping = OpenFileMappingW(
            FILE_MAP_READ,
            FALSE,
            gtaiv_xr_bridge::MappingName);
        if (!mapping)
        {
            logRateLimited(
                "GameBridge: GTA stereo descriptor is not running yet",
                lastMappingLogTick);
            return false;
        }
        shared = static_cast<const FrameBridge*>(MapViewOfFile(
            mapping,
            FILE_MAP_READ,
            0u,
            0u,
            sizeof(FrameBridge)));
        if (!shared)
        {
            releaseMapping();
            logRateLimited(
                "GameBridge: could not map GTA stereo descriptor",
                lastMappingLogTick);
            return false;
        }
        log("GameBridge: GTA frame descriptor v6 found");
        return true;
    }

    bool producerActive(uint32_t expectedPid)
    {
        DWORD exitCode = 0u;
        return producerProcess
            && GetProcessId(producerProcess) == expectedPid
            && WaitForSingleObject(producerProcess, 0u) == WAIT_TIMEOUT
            && GetExitCodeProcess(producerProcess, &exitCode)
            && exitCode == STILL_ACTIVE;
    }

    bool duplicateHandle(uint64_t sourceValue, HANDLE& destination)
    {
        destination = nullptr;
        if (!rawHandleValid(sourceValue))
            return false;
        return DuplicateHandle(
                   producerProcess,
                   reinterpret_cast<HANDLE>(
                       static_cast<uintptr_t>(sourceValue)),
                   GetCurrentProcess(),
                   &destination,
                   0u,
                   FALSE,
                   DUPLICATE_SAME_ACCESS)
            && destination;
    }

    bool textureDescriptionMatches(
        ID3D11Texture2D* texture,
        const FrameBridge& value)
    {
        if (!texture)
            return false;
        D3D11_TEXTURE2D_DESC description {};
        texture->GetDesc(&description);
        return description.Width == value.width
            && description.Height == value.height
            && description.MipLevels == 1u
            && description.ArraySize == 1u
            && description.Format == dxgiFormat(value.format)
            && description.SampleDesc.Count == 1u
            && description.Usage == D3D11_USAGE_DEFAULT
            && (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0u
            && (description.MiscFlags
                & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0u
            && (description.MiscFlags
                & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) == 0u;
    }

    bool createInternalFences()
    {
        HANDLE sharedHandle = nullptr;
        HRESULT result = ingestDevice5->CreateFence(
            0u,
            D3D11_FENCE_FLAG_SHARED,
            IID_PPV_ARGS(&ingestCopyReadyFence));
        if (SUCCEEDED(result) && ingestCopyReadyFence)
        {
            result = ingestCopyReadyFence->CreateSharedHandle(
                nullptr,
                GENERIC_ALL,
                nullptr,
                &sharedHandle);
        }
        if (SUCCEEDED(result) && sharedHandle)
        {
            result = device5->OpenSharedFence(
                sharedHandle,
                IID_PPV_ARGS(&renderCopyReadyFence));
        }
        if (sharedHandle)
        {
            CloseHandle(sharedHandle);
            sharedHandle = nullptr;
        }
        if (FAILED(result)
            || !ingestCopyReadyFence
            || !renderCopyReadyFence)
        {
            return false;
        }

        result = device5->CreateFence(
            0u,
            D3D11_FENCE_FLAG_SHARED,
            IID_PPV_ARGS(&renderPrivateReleaseFence));
        if (SUCCEEDED(result) && renderPrivateReleaseFence)
        {
            result = renderPrivateReleaseFence->CreateSharedHandle(
                nullptr,
                GENERIC_ALL,
                nullptr,
                &sharedHandle);
        }
        if (SUCCEEDED(result) && sharedHandle)
        {
            result = ingestDevice5->OpenSharedFence(
                sharedHandle,
                IID_PPV_ARGS(&ingestPrivateReleaseFence));
        }
        if (sharedHandle)
            CloseHandle(sharedHandle);
        return SUCCEEDED(result)
            && renderPrivateReleaseFence
            && ingestPrivateReleaseFence;
    }

    bool createPrivateTextures(const FrameBridge& value)
    {
        D3D11_TEXTURE2D_DESC description {};
        description.Width = value.width;
        description.Height = value.height;
        description.MipLevels = 1u;
        description.ArraySize = 1u;
        description.Format = privateStorageFormat(value.format);
        description.SampleDesc.Count = 1u;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags =
            D3D11_RESOURCE_MISC_SHARED_NTHANDLE
            | D3D11_RESOURCE_MISC_SHARED;
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription {};
        viewDescription.Format = privateViewFormat(value.format);
        viewDescription.ViewDimension =
            D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDescription.Texture2D.MostDetailedMip = 0u;
        viewDescription.Texture2D.MipLevels = 1u;
        if (description.Format == DXGI_FORMAT_UNKNOWN
            || viewDescription.Format == DXGI_FORMAT_UNKNOWN)
        {
            return false;
        }
        if (!createInternalFences())
            return false;

        for (uint32_t slotIndex = 0u;
             slotIndex < gtaiv_xr_bridge::SlotCount;
             ++slotIndex)
        {
            PrivateSlot& slot = privateSlots[slotIndex];
            for (uint32_t eye = 0u;
                 eye < gtaiv_xr_bridge::EyeCount;
                 ++eye)
            {
                HRESULT result = ingestDevice->CreateTexture2D(
                    &description,
                    nullptr,
                    &slot.ingestTextures[eye]);
                ComPtr<IDXGIResource1> shareable;
                HANDLE textureHandle = nullptr;
                if (SUCCEEDED(result) && slot.ingestTextures[eye])
                    result = slot.ingestTextures[eye].As(&shareable);
                if (SUCCEEDED(result) && shareable)
                {
                    result = shareable->CreateSharedHandle(
                        nullptr,
                        DXGI_SHARED_RESOURCE_READ
                            | DXGI_SHARED_RESOURCE_WRITE,
                        nullptr,
                        &textureHandle);
                }
                if (SUCCEEDED(result) && textureHandle)
                {
                    result = device5->OpenSharedResource1(
                        textureHandle,
                        IID_PPV_ARGS(&slot.renderTextures[eye]));
                }
                if (textureHandle)
                    CloseHandle(textureHandle);
                if (SUCCEEDED(result) && slot.renderTextures[eye])
                {
                    result = device->CreateShaderResourceView(
                        slot.renderTextures[eye].Get(),
                        &viewDescription,
                        &slot.views[eye]);
                }
                if (FAILED(result)
                    || !slot.ingestTextures[eye]
                    || !slot.renderTextures[eye]
                    || !slot.views[eye])
                {
                    return false;
                }
            }
        }
        const bool distinct =
            privateSlots[0].views[0].Get()
                != privateSlots[0].views[1].Get()
            && privateSlots[0].views[0].Get()
                != privateSlots[1].views[0].Get();
        if (distinct)
        {
            log(
                "GameBridge: color path READY "
                "(isolated typeless ring + sRGB decode SRV)");
        }
        return distinct;
    }

    bool openResourceSet(const FrameBridge& value)
    {
        if ((value.flags & gtaiv_xr_bridge::CpuBgraMailbox) != 0u)
            return openCpuMailbox(value);

        releaseResources();
        const uint32_t requiredFlags =
            gtaiv_xr_bridge::Running
            | gtaiv_xr_bridge::GpuD3D11NtHandles
            | gtaiv_xr_bridge::Stereo
            | gtaiv_xr_bridge::EyesDistinct
            | gtaiv_xr_bridge::PoseStamped;
        if (value.adapterLuid != adapterLuid
            || value.width == 0u
            || value.height == 0u
            || dxgiFormat(value.format) == DXGI_FORMAT_UNKNOWN
            || (value.flags & requiredFlags) != requiredFlags
            || !rawHandleValid(value.readyFenceHandle)
            || !rawHandleValid(value.releaseFenceHandle))
        {
            std::ostringstream message;
            message << "GameBridge: producer descriptor rejected; hostLuid="
                    << std::hex << adapterLuid
                    << " producerLuid=" << value.adapterLuid
                    << " flags=0x" << value.flags;
            logRateLimited(message.str(), lastOpenFailureLogTick);
            return false;
        }
        for (const gtaiv_xr_bridge::ResourceSlot& slot : value.slots)
        {
            const uint64_t left = slot.eyes[0].textureHandle;
            const uint64_t right = slot.eyes[1].textureHandle;
            if (!rawHandleValid(left)
                || !rawHandleValid(right)
                || left == right)
            {
                logRateLimited(
                    "GameBridge: aliased or invalid L/R resource handles rejected",
                    lastOpenFailureLogTick);
                return false;
            }
        }

        constexpr DWORD access =
            PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
        producerProcess = OpenProcess(access, FALSE, value.producerPid);
        if (!producerProcess || !producerActive(value.producerPid))
        {
            logRateLimited(
                "GameBridge: GTA producer process is unavailable",
                lastOpenFailureLogTick);
            releaseResources();
            return false;
        }

        using RawHandleSet = std::array<
            std::array<HANDLE, gtaiv_xr_bridge::EyeCount>,
            gtaiv_xr_bridge::SlotCount>;
        RawHandleSet textureHandles {};
        HANDLE readyHandle = nullptr;
        HANDLE releaseHandle = nullptr;
        bool duplicated = true;
        for (uint32_t slot = 0u;
             slot < gtaiv_xr_bridge::SlotCount;
             ++slot)
        {
            for (uint32_t eye = 0u;
                 eye < gtaiv_xr_bridge::EyeCount;
                 ++eye)
            {
                duplicated = duplicated
                    && duplicateHandle(
                        value.slots[slot].eyes[eye].textureHandle,
                        textureHandles[slot][eye]);
            }
        }
        duplicated = duplicated
            && duplicateHandle(value.readyFenceHandle, readyHandle)
            && duplicateHandle(value.releaseFenceHandle, releaseHandle);

        bool opened = duplicated;
        if (opened)
        {
            for (uint32_t slot = 0u;
                 slot < gtaiv_xr_bridge::SlotCount && opened;
                 ++slot)
            {
                for (uint32_t eye = 0u;
                     eye < gtaiv_xr_bridge::EyeCount;
                     ++eye)
                {
                    if (FAILED(ingestDevice5->OpenSharedResource1(
                            textureHandles[slot][eye],
                            IID_PPV_ARGS(&sharedTextures[slot][eye])))
                        || !sharedTextures[slot][eye])
                    {
                        opened = false;
                        break;
                    }
                }
            }
        }
        if (opened)
        {
            opened =
                SUCCEEDED(ingestDevice5->OpenSharedFence(
                    readyHandle,
                    IID_PPV_ARGS(&readyFence)))
                && readyFence
                && SUCCEEDED(ingestDevice5->OpenSharedFence(
                    releaseHandle,
                    IID_PPV_ARGS(&releaseFence)))
                && releaseFence;
        }

        for (const auto& slot : textureHandles)
        {
            for (HANDLE handle : slot)
            {
                if (handle)
                    CloseHandle(handle);
            }
        }
        if (readyHandle)
            CloseHandle(readyHandle);
        if (releaseHandle)
            CloseHandle(releaseHandle);

        if (!opened)
        {
            logRateLimited(
                "GameBridge: opening Vulkan-imported D3D11 resources failed",
                lastOpenFailureLogTick);
            releaseResources();
            return false;
        }
        for (const auto& slot : sharedTextures)
        {
            for (const ComPtr<ID3D11Texture2D>& texture : slot)
            {
                if (!textureDescriptionMatches(texture.Get(), value))
                {
                    logRateLimited(
                        "GameBridge: shared eye texture description mismatch",
                        lastOpenFailureLogTick);
                    releaseResources();
                    return false;
                }
            }
        }
        if (!createPrivateTextures(value))
        {
            logRateLimited(
                "GameBridge: private L/R textures could not be created",
                lastOpenFailureLogTick);
            releaseResources();
            return false;
        }

        connectedPid = value.producerPid;
        connectedEpoch = value.producerEpoch;
        connectedResourceSet = value.resourceSetId;
        connectedGeneration = value.resourceGeneration;
        std::ostringstream message;
        message << "GameBridge: CONNECTED frame source pid=" << connectedPid
                << " source=" << value.width << 'x' << value.height
                << " adapterLuid=" << std::hex << value.adapterLuid
                << " resourceSet=" << value.resourceSetId;
        log(message.str());
        return true;
    }

    bool descriptorLayoutValid(const FrameBridge& value) const
    {
        return value.magic == gtaiv_xr_bridge::Magic
            && value.version == gtaiv_xr_bridge::Version
            && value.structBytes == sizeof(FrameBridge)
            && value.mappingBytes == sizeof(FrameBridge)
            && value.slotCount == gtaiv_xr_bridge::SlotCount
            && value.producerPid != 0u
            && value.producerEpoch != 0u
            && value.resourceSetId != 0u
            && (value.flags & gtaiv_xr_bridge::Running) != 0u;
    }

    bool descriptorHeartbeatFresh(const FrameBridge& value) const
    {
        const uint64_t now = GetTickCount64();
        return value.heartbeatTickMs != 0u
            && now >= value.heartbeatTickMs
            && now - value.heartbeatTickMs <= 1500u;
    }

    bool drainRejectedTransaction(
        const FrameBridge& value,
        const char* reason)
    {
        if (cpuMailboxMode)
        {
            std::ostringstream message;
            message << "GameBridge: CPU mailbox transaction "
                    << value.transactionId
                    << " rejected (" << reason << ')';
            logRateLimited(message.str(), lastQualityRejectLogTick, 2000u);
            lastEnqueuedTransaction = value.transactionId;
            return true;
        }
        if (value.currentSlot >= gtaiv_xr_bridge::SlotCount
            || value.transactionId == 0u
            || value.slots[value.currentSlot].transactionId
                != value.transactionId)
        {
            return false;
        }
        HRESULT result = ingestContext4->Wait(
            readyFence.Get(),
            value.transactionId);
        if (SUCCEEDED(result))
        {
            result = ingestContext4->Signal(
                releaseFence.Get(),
                value.transactionId);
            ingestContext->Flush();
        }
        std::ostringstream message;
        message << "GameBridge: transaction " << value.transactionId
                << " rejected (" << reason << ')';
        logRateLimited(message.str(), lastQualityRejectLogTick, 2000u);
        if (SUCCEEDED(result))
        {
            lastEnqueuedTransaction = value.transactionId;
            current = {};
        }
        return SUCCEEDED(result);
    }

    GameFrameView makeFrame(
        const FrameBridge& value,
        const TransactionQuality& quality,
        uint32_t privateSlot) const
    {
        GameFrameView frame {};
        frame.eyeViews[0] =
            privateSlots[privateSlot].views[0].Get();
        frame.eyeViews[1] =
            privateSlots[privateSlot].views[1].Get();
        frame.width = value.width;
        frame.height = value.height;
        frame.contentWidth = value.contentWidth;
        frame.contentHeight = value.contentHeight;
        frame.transactionId = value.transactionId;
        frame.sameSimulationTick = quality.sameTick;
        frame.temporalStereo = quality.temporalStereo;
        frame.parentDualStereo = quality.parentDualStereo;
        frame.firstPersonCamera = quality.firstPersonCamera;
        frame.nativeHeadHidden = quality.nativeHeadHidden;
        frame.pixelDistinct = quality.pixelDistinct;
        frame.presentationMode = value.presentationMode;
        frame.uiReasonFlags = value.uiReasonFlags;
        frame.uiEye = value.uiEye;
        for (uint32_t eye = 0u;
             eye < gtaiv_xr_bridge::EyeCount;
             ++eye)
        {
            frame.sourceFrameId[eye] = value.sourceFrameId[eye];
            frame.poseSequence[eye] = value.poseSequence[eye];
            frame.renderedDisplayTime[eye] =
                value.renderedDisplayTime[eye];
        }
        return frame;
    }

    GameFrameView makeCpuFrame(
        const FrameBridge& value,
        const TransactionQuality& quality) const
    {
        GameFrameView frame {};
        frame.eyeViews[0] = cpuViews[0].Get();
        frame.eyeViews[1] = cpuViews[1].Get();
        frame.width = value.width;
        frame.height = value.height;
        frame.contentWidth = value.contentWidth;
        frame.contentHeight = value.contentHeight;
        frame.transactionId = value.transactionId;
        frame.sameSimulationTick = quality.sameTick;
        frame.temporalStereo = quality.temporalStereo;
        frame.parentDualStereo = quality.parentDualStereo;
        frame.firstPersonCamera = quality.firstPersonCamera;
        frame.nativeHeadHidden = quality.nativeHeadHidden;
        frame.pixelDistinct = quality.pixelDistinct;
        frame.presentationMode = value.presentationMode;
        frame.uiReasonFlags = value.uiReasonFlags;
        frame.uiEye = value.uiEye;
        for (uint32_t eye = 0u;
             eye < gtaiv_xr_bridge::EyeCount;
             ++eye)
        {
            frame.sourceFrameId[eye] = value.sourceFrameId[eye];
            frame.poseSequence[eye] = value.poseSequence[eye];
            frame.renderedDisplayTime[eye] =
                value.renderedDisplayTime[eye];
        }
        return frame;
    }

    bool consumeCpuMailbox(
        const FrameBridge& value,
        const TransactionQuality& quality)
    {
        if (!cpuMailboxMode
            || !cpuMapping
            || value.currentSlot >= gtaiv_xr_bridge::SlotCount
            || value.width == 0u
            || value.height == 0u
            || value.width > gtaiv_xr_bridge::CpuFrameMaxWidth
            || value.height > gtaiv_xr_bridge::CpuFrameMaxHeight)
        {
            return false;
        }
        for (const ComPtr<ID3D11Texture2D>& texture : cpuTextures)
        {
            if (!texture)
                return false;
        }

        const uint32_t slot = value.currentSlot;
        const volatile LONG* sequence =
            reinterpret_cast<const volatile LONG*>(
                &cpuMapping->slotSequence[slot]);
        const LONG before = *sequence;
        if (before == 0 || (before & 1) != 0)
            return false;
        MemoryBarrier();
        const uint64_t mailboxTransaction =
            cpuMapping->slotTransactionId[slot];
        if (mailboxTransaction != value.transactionId
            || value.slots[slot].transactionId != value.transactionId)
        {
            return false;
        }

        const size_t rowBytes = static_cast<size_t>(value.width)
            * gtaiv_xr_bridge::CpuFrameBytesPerPixel;
        const size_t frameBytes = rowBytes * value.height;
        if (frameBytes == 0u
            || frameBytes > gtaiv_xr_bridge::CpuFrameEyeBytes)
        {
            return false;
        }
        const bool distinctStereo = value.presentationMode
            == gtaiv_xr_bridge::PresentationMode::WorldStereo;
        const uint32_t sourceEyeCount = distinctStereo
            ? gtaiv_xr_bridge::EyeCount
            : 1u;
        const auto* slotPixels = reinterpret_cast<const uint8_t*>(cpuMapping)
            + sizeof(gtaiv_xr_bridge::CpuFrameMailbox)
            + static_cast<size_t>(slot)
                * static_cast<size_t>(gtaiv_xr_bridge::CpuFrameSlotBytes);
        for (uint32_t eye = 0u; eye < sourceEyeCount; ++eye)
        {
            cpuPixels[eye].resize(frameBytes);
            const auto* source = slotPixels
                + static_cast<size_t>(eye)
                    * static_cast<size_t>(gtaiv_xr_bridge::CpuFrameEyeBytes);
            std::memcpy(cpuPixels[eye].data(), source, frameBytes);
        }
        if (!distinctStereo)
        {
            cpuPixels[1] = cpuPixels[0];
        }
        const uint64_t started = GetTickCount64();
        MemoryBarrier();
        const LONG after = *sequence;
        const uint64_t verifiedTransaction =
            cpuMapping->slotTransactionId[slot];
        if (before != after
            || (after & 1) != 0
            || verifiedTransaction != value.transactionId)
        {
            return false;
        }

        for (uint32_t eye = 0u;
             eye < gtaiv_xr_bridge::EyeCount;
             ++eye)
        {
            context->UpdateSubresource(
                cpuTextures[eye].Get(),
                0u,
                nullptr,
                cpuPixels[eye].data(),
                static_cast<UINT>(rowBytes),
                0u);
        }
        current = makeCpuFrame(value, quality);
        lastEnqueuedTransaction = value.transactionId;
        lastCpuMailboxTransaction = value.transactionId;
        ++acquiredFrameCount;

        if (acquiredFrameCount <= 4u
            || current.transactionId % 60u == 0u
            || current.presentationMode != lastLoggedCpuPresentationMode)
        {
            std::ostringstream message;
            message << "GameBridge: CPU mailbox acquired transaction="
                    << current.transactionId
                    << " slot=" << slot
                    << " presentation="
                    << (current.presentationMode
                                == gtaiv_xr_bridge::PresentationMode::UiQuad
                            ? "stationary-ui-quad"
                            : current.presentationMode
                                    == gtaiv_xr_bridge::PresentationMode::WorldStereo
                                ? current.parentDualStereo
                                    ? "world-parent-dual-stereo"
                                    : current.temporalStereo
                                    ? "world-temporal-stereo"
                                    : "world-drawscene-stereo"
                                : "world-immersive-mono")
                    << " eyes=" << sourceEyeCount
                    << " sameTick="
                    << (current.sameSimulationTick ? 1 : 0)
                    << " temporal="
                    << (current.temporalStereo ? 1 : 0)
                    << " parentDual="
                    << (current.parentDualStereo ? 1 : 0)
                    << " fp="
                    << (current.firstPersonCamera ? 1 : 0)
                    << " headHide="
                    << (current.nativeHeadHidden ? 1 : 0)
                    << " pixelDistinct="
                    << (current.pixelDistinct ? 1 : 0)
                    << " copyMs=" << (GetTickCount64() - started);
            log(message.str());
            lastLoggedCpuPresentationMode = current.presentationMode;
        }
        return true;
    }

    bool promoteReadyFrame()
    {
        if (pendingPrivateSlot >= gtaiv_xr_bridge::SlotCount)
            return false;
        const uint64_t transaction =
            privateSlots[pendingPrivateSlot].transactionId;
        const uint64_t completed =
            renderCopyReadyFence->GetCompletedValue();
        if (completed == (std::numeric_limits<uint64_t>::max)())
        {
            log("GameBridge: isolated copy fence reported device removal");
            releaseResources();
            return false;
        }
        if (completed < transaction)
            return false;

        const gtaiv_xr_bridge::PresentationMode priorMode =
            current.presentationMode;
        if (currentPrivateSlot < gtaiv_xr_bridge::SlotCount)
        {
            const uint64_t oldTransaction =
                privateSlots[currentPrivateSlot].transactionId;
            if (FAILED(context4->Signal(
                    renderPrivateReleaseFence.Get(),
                    oldTransaction)))
            {
                log("GameBridge: private-ring release signal failed");
                releaseResources();
                return false;
            }
        }
        if (FAILED(context4->Wait(
                renderCopyReadyFence.Get(),
                transaction)))
        {
            log("GameBridge: private-ring ready wait failed");
            releaseResources();
            return false;
        }
        context->Flush();

        currentPrivateSlot = pendingPrivateSlot;
        pendingPrivateSlot = gtaiv_xr_bridge::SlotCount;
        current = pendingFrame;
        pendingFrame = {};
        const uint64_t latency =
            pendingEnqueueTick != 0u
                ? GetTickCount64() - pendingEnqueueTick
                : 0u;
        pendingEnqueueTick = 0u;
        ++acquiredFrameCount;

        if (acquiredFrameCount <= 4u
            || acquiredFrameCount % 120u == 0u
            || current.presentationMode != priorMode)
        {
            std::ostringstream message;
            message << "GameBridge: frame acquired transaction="
                    << current.transactionId
                    << " privateSlot=" << currentPrivateSlot
                    << " sameTick="
                    << (current.sameSimulationTick ? 1 : 0)
                    << " temporal="
                    << (current.temporalStereo ? 1 : 0)
                    << " parentDual="
                    << (current.parentDualStereo ? 1 : 0)
                    << " fp="
                    << (current.firstPersonCamera ? 1 : 0)
                    << " headHide="
                    << (current.nativeHeadHidden ? 1 : 0)
                    << " pixelDistinct="
                    << (current.pixelDistinct ? 1 : 0)
                    << " presentation="
                    << (current.presentationMode
                                == gtaiv_xr_bridge::PresentationMode::UiQuad
                            ? "stationary-ui-quad"
                            : current.presentationMode
                                    == gtaiv_xr_bridge::PresentationMode::WorldMono
                                ? "world-immersive-mono"
                                : current.parentDualStereo
                                    ? "world-parent-dual-stereo"
                                    : current.temporalStereo
                                        ? "world-temporal-stereo"
                                        : "world-stereo")
                    << " pose=" << current.poseSequence[0]
                    << '/' << current.poseSequence[1]
                    << " ingestLatencyMs=" << latency;
            log(message.str());
        }
        return true;
    }

    uint32_t findReusablePrivateSlot()
    {
        const uint64_t released =
            ingestPrivateReleaseFence->GetCompletedValue();
        if (released == (std::numeric_limits<uint64_t>::max)())
            return gtaiv_xr_bridge::SlotCount;
        for (uint32_t slotIndex = 0u;
             slotIndex < gtaiv_xr_bridge::SlotCount;
             ++slotIndex)
        {
            if (slotIndex == currentPrivateSlot
                || slotIndex == pendingPrivateSlot)
            {
                continue;
            }
            const uint64_t previous =
                privateSlots[slotIndex].transactionId;
            if (previous == 0u || released >= previous)
                return slotIndex;
        }
        return gtaiv_xr_bridge::SlotCount;
    }

    bool enqueueIsolatedCopy(
        const FrameBridge& value,
        const TransactionQuality& quality)
    {
        if (pendingPrivateSlot < gtaiv_xr_bridge::SlotCount)
            return false;
        const uint32_t privateSlot = findReusablePrivateSlot();
        if (privateSlot >= gtaiv_xr_bridge::SlotCount)
        {
            logRateLimited(
                "GameBridge: isolated private ring busy; holding last frame",
                lastPrivateRingLogTick,
                2000u);
            return false;
        }

        PrivateSlot& destination = privateSlots[privateSlot];
        const uint64_t previous = destination.transactionId;
        HRESULT result = S_OK;
        if (previous != 0u)
        {
            result = ingestContext4->Wait(
                ingestPrivateReleaseFence.Get(),
                previous);
        }
        if (SUCCEEDED(result))
        {
            result = ingestContext4->Wait(
                readyFence.Get(),
                value.transactionId);
        }

        const bool singleEyeTransport =
            quality.uiQuad || quality.immersiveMono;
        const uint32_t firstEye =
            quality.uiQuad ? value.uiEye : 0u;
        const uint32_t copyCount =
            singleEyeTransport ? 1u : gtaiv_xr_bridge::EyeCount;
        if (SUCCEEDED(result))
        {
            for (uint32_t copyIndex = 0u;
                 copyIndex < copyCount;
                 ++copyIndex)
            {
                const uint32_t eye =
                    singleEyeTransport ? firstEye : copyIndex;
                ingestContext->CopyResource(
                    destination.ingestTextures[eye].Get(),
                    sharedTextures[value.currentSlot][eye].Get());
            }
            result = ingestContext4->Signal(
                releaseFence.Get(),
                value.transactionId);
        }
        if (SUCCEEDED(result))
        {
            result = ingestContext4->Signal(
                ingestCopyReadyFence.Get(),
                value.transactionId);
        }
        if (FAILED(result))
        {
            logRateLimited(
                "GameBridge: isolated GPU ingest submission failed",
                lastConsumeFailureLogTick);
            return false;
        }
        ingestContext->Flush();

        destination.transactionId = value.transactionId;
        pendingPrivateSlot = privateSlot;
        pendingFrame = makeFrame(value, quality, privateSlot);
        pendingEnqueueTick = GetTickCount64();
        lastEnqueuedTransaction = value.transactionId;
        return true;
    }

    GameFrameView update()
    {
        promoteReadyFrame();
        if (!ensureMapping())
            return current;

        FrameBridge value {};
        if (!stableSnapshot(shared, value))
            return current;
        if (!descriptorLayoutValid(value))
        {
            if (connectedPid != 0u)
            {
                log("GameBridge: GTA producer stopped; waiting for a new descriptor");
                releaseResources();
            }
            return current;
        }
        const bool changed =
            connectedPid != value.producerPid
            || connectedEpoch != value.producerEpoch
            || connectedResourceSet != value.resourceSetId
            || connectedGeneration != value.resourceGeneration;
        if (changed && !openResourceSet(value))
            return current;
        if (!producerActive(value.producerPid))
        {
            log("GameBridge: GTAIV.exe exited; waiting for restart");
            releaseResources();
            releaseMapping();
            return current;
        }
        if (!descriptorHeartbeatFresh(value))
        {
            logRateLimited(
                "GameBridge: GTA frame heartbeat paused; holding the last "
                "frame and isolated resource ring",
                lastHeartbeatPauseLogTick,
                2000u);
            return current;
        }
        if (value.transactionId == 0u
            || value.transactionId == lastEnqueuedTransaction
            || pendingPrivateSlot < gtaiv_xr_bridge::SlotCount)
        {
            return current;
        }
        if (value.transactionId < lastEnqueuedTransaction
            || value.currentSlot >= gtaiv_xr_bridge::SlotCount
            || value.slots[value.currentSlot].transactionId
                != value.transactionId)
        {
            logRateLimited(
                "GameBridge: stale or incomplete transaction rejected",
                lastConsumeFailureLogTick);
            return current;
        }

        const TransactionQuality quality =
            assessTransactionQuality(value, allowTemporalStereo);
        if (!quality.accepted)
        {
            drainRejectedTransaction(value, quality.rejection);
            return current;
        }
        if (cpuMailboxMode)
        {
            if (!consumeCpuMailbox(value, quality))
            {
                logRateLimited(
                    "GameBridge: CPU mailbox transaction not stable yet; "
                    "holding last frame",
                    lastConsumeFailureLogTick,
                    2000u);
            }
            return current;
        }
        enqueueIsolatedCopy(value, quality);
        return current;
    }

    LogFunction logger;
    const bool allowTemporalStereo = false;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Device> ingestDevice;
    ComPtr<ID3D11Device5> ingestDevice5;
    ComPtr<ID3D11DeviceContext> ingestContext;
    ComPtr<ID3D11DeviceContext4> ingestContext4;
    uint64_t adapterLuid = 0u;

    HANDLE mapping = nullptr;
    const FrameBridge* shared = nullptr;
    HANDLE producerProcess = nullptr;
    HANDLE cpuMappingHandle = nullptr;
    const gtaiv_xr_bridge::CpuFrameMailbox* cpuMapping = nullptr;
    bool cpuMailboxMode = false;
    std::array<ComPtr<ID3D11Texture2D>, gtaiv_xr_bridge::EyeCount>
        cpuTextures;
    std::array<ComPtr<ID3D11ShaderResourceView>, gtaiv_xr_bridge::EyeCount>
        cpuViews;
    std::array<std::vector<uint8_t>, gtaiv_xr_bridge::EyeCount>
        cpuPixels;
    SharedTextureSet sharedTextures;
    ComPtr<ID3D11Fence> readyFence;
    ComPtr<ID3D11Fence> releaseFence;
    std::array<PrivateSlot, gtaiv_xr_bridge::SlotCount> privateSlots;
    ComPtr<ID3D11Fence> ingestCopyReadyFence;
    ComPtr<ID3D11Fence> renderCopyReadyFence;
    ComPtr<ID3D11Fence> renderPrivateReleaseFence;
    ComPtr<ID3D11Fence> ingestPrivateReleaseFence;
    GameFrameView current {};
    GameFrameView pendingFrame {};

    uint32_t connectedPid = 0u;
    uint64_t connectedEpoch = 0u;
    uint64_t connectedResourceSet = 0u;
    uint32_t connectedGeneration = 0u;
    uint64_t lastEnqueuedTransaction = 0u;
    uint64_t lastCpuMailboxTransaction = 0u;
    uint64_t pendingEnqueueTick = 0u;
    uint64_t acquiredFrameCount = 0u;
    uint32_t currentPrivateSlot = gtaiv_xr_bridge::SlotCount;
    uint32_t pendingPrivateSlot = gtaiv_xr_bridge::SlotCount;
    uint64_t lastMappingLogTick = 0u;
    uint64_t lastOpenFailureLogTick = 0u;
    uint64_t lastConsumeFailureLogTick = 0u;
    uint64_t lastHeartbeatPauseLogTick = 0u;
    uint64_t lastPrivateRingLogTick = 0u;
    uint64_t lastQualityRejectLogTick = 0u;
    gtaiv_xr_bridge::PresentationMode lastLoggedCpuPresentationMode =
        gtaiv_xr_bridge::PresentationMode::Unknown;
};

GameBridge::GameBridge() = default;

GameBridge::~GameBridge()
{
    reset();
}

bool GameBridge::initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    LogFunction logger,
    bool allowTemporalStereo)
{
    reset();
    implementation_ = new (std::nothrow) Implementation(
        std::move(logger),
        allowTemporalStereo);
    if (!implementation_)
        return false;
    if (!implementation_->initialize(device, context))
    {
        reset();
        return false;
    }
    return true;
}

GameFrameView GameBridge::update()
{
    return implementation_ ? implementation_->update() : GameFrameView {};
}

void GameBridge::reset()
{
    delete implementation_;
    implementation_ = nullptr;
}

bool GameBridgeProtocolSelfTest(std::string& failure)
{
    FrameBridge value {};
    value.flags =
        gtaiv_xr_bridge::Running
        | gtaiv_xr_bridge::GpuD3D11NtHandles
        | gtaiv_xr_bridge::Stereo
        | gtaiv_xr_bridge::EyesDistinct
        | gtaiv_xr_bridge::PoseStamped
        | gtaiv_xr_bridge::SameSimulationTick
        | gtaiv_xr_bridge::VerifiedWvpStereo;
    value.presentationMode =
        gtaiv_xr_bridge::PresentationMode::WorldStereo;
    value.sourceFrameId[0] = 10u;
    value.sourceFrameId[1] = 10u;
    value.poseSequence[0] = 20u;
    value.poseSequence[1] = 20u;
    value.renderedDisplayTime[0] = 30;
    value.renderedDisplayTime[1] = 30;
    value.contentWidth = 1920u;
    value.contentHeight = 1080u;
    value.uiEye = 0u;
    if (!assessTransactionQuality(value, false).accepted)
    {
        failure = "strict same-tick world pair was rejected";
        return false;
    }

    value.flags &= ~gtaiv_xr_bridge::VerifiedWvpStereo;
    value.flags |= gtaiv_xr_bridge::VerifiedDrawSceneStereo;
    if (!assessTransactionQuality(value, false).accepted)
    {
        failure = "strict DrawScene-proof world pair was rejected";
        return false;
    }
    value.flags &= ~gtaiv_xr_bridge::VerifiedDrawSceneStereo;

    value.flags |=
        gtaiv_xr_bridge::VerifiedParentDualStereo;
    TransactionQuality parentDualQuality =
        assessTransactionQuality(value, false);
    if (!parentDualQuality.accepted
        || !parentDualQuality.sameTick
        || parentDualQuality.temporalStereo
        || !parentDualQuality.parentDualStereo
        || parentDualQuality.firstPersonCamera
        || parentDualQuality.nativeHeadHidden
        || !parentDualQuality.pixelDistinct)
    {
        failure =
            "literal Mode204 parent-dual submit pair was rejected";
        return false;
    }
    GameFrameView parentDualFrame {};
    parentDualFrame.parentDualStereo =
        parentDualQuality.parentDualStereo;
    parentDualFrame.temporalStereo =
        parentDualQuality.temporalStereo;
    if (!parentDualFrame.requiresExactCapturePose())
    {
        failure =
            "parent-dual world route did not require exact capture pose";
        return false;
    }
    value.flags |=
        gtaiv_xr_bridge::FirstPersonCamera
        | gtaiv_xr_bridge::NativeHeadHidden;
    parentDualQuality =
        assessTransactionQuality(value, false);
    if (!parentDualQuality.accepted
        || !parentDualQuality.parentDualStereo
        || !parentDualQuality.firstPersonCamera
        || !parentDualQuality.nativeHeadHidden)
    {
        failure =
            "parent-dual observational camera/head telemetry was lost";
        return false;
    }
    value.flags &=
        ~(gtaiv_xr_bridge::FirstPersonCamera
            | gtaiv_xr_bridge::NativeHeadHidden);
    parentDualQuality =
        assessTransactionQuality(value, false);
    if (!parentDualQuality.accepted
        || !parentDualQuality.parentDualStereo
        || parentDualQuality.firstPersonCamera
        || parentDualQuality.nativeHeadHidden)
    {
        failure =
            "optional camera/head telemetry changed Mode204 acceptance";
        return false;
    }
    value.flags &= ~gtaiv_xr_bridge::SameSimulationTick;
    if (assessTransactionQuality(value, false).accepted)
    {
        failure = "non-same-tick parent-dual world pair was accepted";
        return false;
    }
    value.flags |= gtaiv_xr_bridge::SameSimulationTick;
    value.flags &=
        ~(gtaiv_xr_bridge::VerifiedParentDualStereo
            | gtaiv_xr_bridge::FirstPersonCamera
            | gtaiv_xr_bridge::NativeHeadHidden);

    if (assessTransactionQuality(value, false).accepted)
    {
        failure = "unverified same-tick world pair was accepted";
        return false;
    }
    value.flags &= ~gtaiv_xr_bridge::VerifiedWvpStereo;
    value.flags &= ~gtaiv_xr_bridge::SameSimulationTick;
    value.flags |= gtaiv_xr_bridge::TemporalStereo;
    value.sourceFrameId[1] = 11u;
    value.poseSequence[1] = 21u;
    value.renderedDisplayTime[1] = 31;
    if (assessTransactionQuality(value, false).accepted)
    {
        failure = "temporal world pair was accepted";
        return false;
    }
    const TransactionQuality temporalQuality =
        assessTransactionQuality(value, true);
    if (!temporalQuality.accepted
        || !temporalQuality.temporalStereo
        || temporalQuality.sameTick)
    {
        failure =
            "explicitly enabled ordered temporal world pair was rejected";
        return false;
    }

    value.flags |= gtaiv_xr_bridge::VerifiedWvpStereo;
    if (assessTransactionQuality(value, true).accepted)
    {
        failure =
            "temporal world pair claiming same-frame WVP proof was accepted";
        return false;
    }
    value.flags &= ~gtaiv_xr_bridge::VerifiedWvpStereo;
    value.poseSequence[1] = value.poseSequence[0];
    if (assessTransactionQuality(value, true).accepted)
    {
        failure = "temporal world pair with repeated pose was accepted";
        return false;
    }
    value.poseSequence[1] = 21u;
    value.flags |= gtaiv_xr_bridge::SameSimulationTick;
    if (assessTransactionQuality(value, true).accepted)
    {
        failure =
            "world pair claiming both same-tick and temporal timing was accepted";
        return false;
    }
    value.flags &= ~gtaiv_xr_bridge::SameSimulationTick;

    value.flags |=
        gtaiv_xr_bridge::VerifiedParentDualStereo
        | gtaiv_xr_bridge::FirstPersonCamera
        | gtaiv_xr_bridge::NativeHeadHidden;
    if (assessTransactionQuality(value, true).accepted)
    {
        failure = "temporal parent-dual world pair was accepted";
        return false;
    }
    value.flags &=
        ~(gtaiv_xr_bridge::VerifiedParentDualStereo
            | gtaiv_xr_bridge::FirstPersonCamera
            | gtaiv_xr_bridge::NativeHeadHidden);

    value.presentationMode =
        gtaiv_xr_bridge::PresentationMode::WorldMono;
    value.flags =
        gtaiv_xr_bridge::Running
        | gtaiv_xr_bridge::GpuD3D11NtHandles
        | gtaiv_xr_bridge::Stereo
        | gtaiv_xr_bridge::EyesDistinct
        | gtaiv_xr_bridge::PoseStamped
        | gtaiv_xr_bridge::SameSimulationTick
        | gtaiv_xr_bridge::ImmersiveMono;
    value.sourceFrameId[0] = 40u;
    value.sourceFrameId[1] = 40u;
    value.poseSequence[0] = 50u;
    value.poseSequence[1] = 50u;
    value.renderedDisplayTime[0] = 60;
    value.renderedDisplayTime[1] = 60;
    value.uiReasonFlags = gtaiv_xr_bridge::UiReasonNone;
    const TransactionQuality monoQuality =
        assessTransactionQuality(value, false);
    if (!monoQuality.accepted || !monoQuality.immersiveMono)
    {
        failure = "valid immersive mono transaction was rejected";
        return false;
    }
    value.flags &= ~gtaiv_xr_bridge::ImmersiveMono;
    if (assessTransactionQuality(value, false).accepted)
    {
        failure = "unmarked immersive mono transaction was accepted";
        return false;
    }
    value.flags |= gtaiv_xr_bridge::ImmersiveMono;
    value.flags |= gtaiv_xr_bridge::VerifiedWvpStereo;
    if (assessTransactionQuality(value, false).accepted)
    {
        failure = "immersive mono transaction claimed stereo WVP proof";
        return false;
    }
    value.flags &= ~gtaiv_xr_bridge::VerifiedWvpStereo;
    value.flags |= gtaiv_xr_bridge::VerifiedDrawSceneStereo;
    if (assessTransactionQuality(value, false).accepted)
    {
        failure = "immersive mono transaction claimed DrawScene stereo proof";
        return false;
    }
    value.flags &= ~gtaiv_xr_bridge::VerifiedDrawSceneStereo;
    value.flags |=
        gtaiv_xr_bridge::VerifiedParentDualStereo
        | gtaiv_xr_bridge::FirstPersonCamera
        | gtaiv_xr_bridge::NativeHeadHidden;
    if (assessTransactionQuality(value, false).accepted)
    {
        failure = "immersive mono transaction claimed parent-dual proof";
        return false;
    }

    value.flags =
        gtaiv_xr_bridge::Running
        | gtaiv_xr_bridge::CpuBgraMailbox
        | gtaiv_xr_bridge::PoseStamped
        | gtaiv_xr_bridge::SameSimulationTick
        | gtaiv_xr_bridge::ImmersiveMono;
    value.presentationMode =
        gtaiv_xr_bridge::PresentationMode::WorldMono;
    const TransactionQuality cpuMonoQuality =
        assessTransactionQuality(value, false);
    if (!cpuMonoQuality.accepted || !cpuMonoQuality.immersiveMono)
    {
        failure = "FNVVR-style CPU immersive mono transaction was rejected";
        return false;
    }
    value.flags =
        gtaiv_xr_bridge::Running
        | gtaiv_xr_bridge::CpuBgraMailbox
        | gtaiv_xr_bridge::Stereo
        | gtaiv_xr_bridge::EyesDistinct
        | gtaiv_xr_bridge::PoseStamped
        | gtaiv_xr_bridge::SameSimulationTick
        | gtaiv_xr_bridge::VerifiedDrawSceneStereo;
    value.presentationMode =
        gtaiv_xr_bridge::PresentationMode::WorldStereo;
    if (!assessTransactionQuality(value, false).accepted)
    {
        failure = "CPU DrawScene stereo transaction was rejected";
        return false;
    }
    value.flags &= ~gtaiv_xr_bridge::VerifiedDrawSceneStereo;
    if (assessTransactionQuality(value, false).accepted)
    {
        failure = "CPU world stereo without a proof was accepted";
        return false;
    }
    value.presentationMode =
        gtaiv_xr_bridge::PresentationMode::UiQuad;
    value.flags =
        gtaiv_xr_bridge::Running
        | gtaiv_xr_bridge::GpuD3D11NtHandles
        | gtaiv_xr_bridge::Stereo
        | gtaiv_xr_bridge::EyesDistinct
        | gtaiv_xr_bridge::PoseStamped
        | gtaiv_xr_bridge::TemporalStereo;
    value.sourceFrameId[0] = 70u;
    value.sourceFrameId[1] = 71u;
    value.poseSequence[0] = 80u;
    value.poseSequence[1] = 81u;
    value.renderedDisplayTime[0] = 90;
    value.renderedDisplayTime[1] = 91;
    value.uiReasonFlags = gtaiv_xr_bridge::UiReasonPauseOrMap;
    value.uiEye = 1u;
    const TransactionQuality uiQuality =
        assessTransactionQuality(value, false);
    if (!uiQuality.accepted || !uiQuality.uiQuad)
    {
        failure = "valid temporal UI quad transaction was rejected";
        return false;
    }
    value.flags |=
        gtaiv_xr_bridge::VerifiedParentDualStereo
        | gtaiv_xr_bridge::FirstPersonCamera
        | gtaiv_xr_bridge::NativeHeadHidden;
    if (assessTransactionQuality(value, false).accepted)
    {
        failure = "UI quad transaction claimed parent-dual proof";
        return false;
    }
    value.flags &=
        ~(gtaiv_xr_bridge::VerifiedParentDualStereo
            | gtaiv_xr_bridge::FirstPersonCamera
            | gtaiv_xr_bridge::NativeHeadHidden);

    value.contentWidth = 0u;
    if (assessTransactionQuality(value, false).accepted)
    {
        failure = "UI quad without source aspect was accepted";
        return false;
    }
    value.contentWidth = 1920u;
    value.uiReasonFlags = gtaiv_xr_bridge::UiReasonNone;
    if (assessTransactionQuality(value, false).accepted)
    {
        failure = "reasonless UI quad transaction was accepted";
        return false;
    }
    if (privateStorageFormat(
            gtaiv_xr_bridge::PixelFormat::B8G8R8A8Unorm)
            != DXGI_FORMAT_B8G8R8A8_TYPELESS
        || privateViewFormat(
            gtaiv_xr_bridge::PixelFormat::B8G8R8A8Unorm)
            != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
        || privateStorageFormat(
            gtaiv_xr_bridge::PixelFormat::R8G8B8A8Unorm)
            != DXGI_FORMAT_R8G8B8A8_TYPELESS
        || privateViewFormat(
            gtaiv_xr_bridge::PixelFormat::R8G8B8A8Unorm)
            != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        failure = "game texture sRGB decode view mapping is incorrect";
        return false;
    }
    if (!colorPathSelfTest())
    {
        failure =
            "D3D11 typed-to-typeless sRGB color path failed";
        return false;
    }
    failure.clear();
    return true;
}

bool GameBridgeCpuMailboxSelfTest(std::string& failure)
{
    constexpr uint32_t TestWidth = 4u;
    constexpr uint32_t TestHeight = 2u;
    HANDLE descriptorHandle = nullptr;
    FrameBridge* descriptor = nullptr;
    HANDLE cpuHandle = nullptr;
    gtaiv_xr_bridge::CpuFrameMailbox* mailbox = nullptr;
    GameBridge bridge;
    auto cleanup = [&]() {
        bridge.reset();
        if (mailbox)
        {
            UnmapViewOfFile(mailbox);
            mailbox = nullptr;
        }
        if (cpuHandle)
        {
            CloseHandle(cpuHandle);
            cpuHandle = nullptr;
        }
        if (descriptor)
        {
            UnmapViewOfFile(descriptor);
            descriptor = nullptr;
        }
        if (descriptorHandle)
        {
            CloseHandle(descriptorHandle);
            descriptorHandle = nullptr;
        }
    };

    descriptorHandle = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0u,
        sizeof(FrameBridge),
        gtaiv_xr_bridge::MappingName);
    const DWORD descriptorCreateError = GetLastError();
    if (!descriptorHandle || descriptorCreateError == ERROR_ALREADY_EXISTS)
    {
        if (descriptorHandle)
            CloseHandle(descriptorHandle);
        failure = "CPU mailbox self-test requires an unused frame descriptor name";
        return false;
    }
    descriptor = static_cast<FrameBridge*>(MapViewOfFile(
        descriptorHandle,
        FILE_MAP_READ | FILE_MAP_WRITE,
        0u,
        0u,
        sizeof(FrameBridge)));
    if (!descriptor)
    {
        cleanup();
        failure = "CPU mailbox self-test could not map the frame descriptor";
        return false;
    }

    cpuHandle = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0u,
        static_cast<DWORD>(gtaiv_xr_bridge::CpuFrameMappingBytes),
        gtaiv_xr_bridge::CpuFrameMappingName);
    const DWORD cpuCreateError = GetLastError();
    if (!cpuHandle || cpuCreateError == ERROR_ALREADY_EXISTS)
    {
        if (cpuHandle)
        {
            CloseHandle(cpuHandle);
            cpuHandle = nullptr;
        }
        cleanup();
        failure = "CPU mailbox self-test requires an unused pixel mailbox name";
        return false;
    }
    mailbox = static_cast<gtaiv_xr_bridge::CpuFrameMailbox*>(MapViewOfFile(
        cpuHandle,
        FILE_MAP_READ | FILE_MAP_WRITE,
        0u,
        0u,
        static_cast<SIZE_T>(gtaiv_xr_bridge::CpuFrameMappingBytes)));
    if (!mailbox)
    {
        cleanup();
        failure = "CPU mailbox self-test could not map pixel storage";
        return false;
    }

    std::memset(mailbox, 0, sizeof(*mailbox));
    mailbox->magic = gtaiv_xr_bridge::CpuFrameMagic;
    mailbox->version = gtaiv_xr_bridge::CpuFrameVersion;
    mailbox->headerBytes = sizeof(*mailbox);
    mailbox->slotCount = gtaiv_xr_bridge::SlotCount;
    mailbox->maxWidth = gtaiv_xr_bridge::CpuFrameMaxWidth;
    mailbox->maxHeight = gtaiv_xr_bridge::CpuFrameMaxHeight;
    mailbox->bytesPerPixel = gtaiv_xr_bridge::CpuFrameBytesPerPixel;
    mailbox->eyeCount = gtaiv_xr_bridge::CpuFrameEyeCount;
    mailbox->mappingBytes = gtaiv_xr_bridge::CpuFrameMappingBytes;
    auto* pixels = reinterpret_cast<uint8_t*>(mailbox)
        + sizeof(gtaiv_xr_bridge::CpuFrameMailbox);
    const uint8_t expected[] = { 0x12u, 0x34u, 0x56u, 0xffu };
    std::memcpy(pixels, expected, sizeof(expected));
    mailbox->slotTransactionId[0] = 1u;
    MemoryBarrier();
    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&mailbox->slotSequence[0]),
        2);

    FrameBridge value {};
    value.magic = gtaiv_xr_bridge::Magic;
    value.version = gtaiv_xr_bridge::Version;
    value.structBytes = sizeof(FrameBridge);
    value.slotCount = gtaiv_xr_bridge::SlotCount;
    value.publicationSequence = 2;
    value.producerPid = GetCurrentProcessId();
    value.producerEpoch = 1u;
    value.resourceSetId = 1u;
    value.resourceGeneration = 1u;
    value.width = TestWidth;
    value.height = TestHeight;
    value.format = gtaiv_xr_bridge::PixelFormat::B8G8R8A8Unorm;
    value.flags = gtaiv_xr_bridge::Running
        | gtaiv_xr_bridge::CpuBgraMailbox
        | gtaiv_xr_bridge::PoseStamped
        | gtaiv_xr_bridge::SameSimulationTick
        | gtaiv_xr_bridge::ImmersiveMono;
    value.currentSlot = 0u;
    value.transactionId = 1u;
    value.sourceFrameId[0] = 1u;
    value.sourceFrameId[1] = 1u;
    value.poseSequence[0] = 1u;
    value.poseSequence[1] = 1u;
    value.renderedDisplayTime[0] = 1;
    value.renderedDisplayTime[1] = 1;
    value.heartbeatTickMs = GetTickCount64();
    value.slots[0].transactionId = 1u;
    value.mappingBytes = sizeof(FrameBridge);
    value.presentationMode = gtaiv_xr_bridge::PresentationMode::WorldMono;
    value.contentWidth = TestWidth;
    value.contentHeight = TestHeight;
    std::memcpy(descriptor, &value, sizeof(value));
    MemoryBarrier();

    const D3D_FEATURE_LEVEL requested[] = { D3D_FEATURE_LEVEL_11_0 };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            requested,
            1u,
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            &context))
        || !device
        || !context
        || !bridge.initialize(
            device.Get(),
            context.Get(),
            [](const std::string&) {},
            false))
    {
        cleanup();
        failure = "CPU mailbox self-test could not initialize WARP ingestion";
        return false;
    }

    const GameFrameView frame = bridge.update();
    if (!frame
        || frame.width != TestWidth
        || frame.height != TestHeight
        || frame.presentationMode
            != gtaiv_xr_bridge::PresentationMode::WorldMono)
    {
        cleanup();
        failure = "CPU mailbox self-test did not publish a frame";
        return false;
    }

    ComPtr<ID3D11Resource> initialResource;
    frame.eyeViews[0]->GetResource(&initialResource);
    ComPtr<ID3D11Texture2D> initialTexture;
    D3D11_TEXTURE2D_DESC description {};
    if (!initialResource
        || FAILED(initialResource.As(&initialTexture))
        || !initialTexture)
    {
        cleanup();
        failure = "CPU mailbox self-test could not inspect uploaded texture";
        return false;
    }
    initialTexture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0u;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0u;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &staging))
        || !staging)
    {
        cleanup();
        failure = "CPU mailbox self-test could not create texture readback";
        return false;
    }
    const auto pixelsMatch = [&](ID3D11ShaderResourceView* view,
                                 const uint8_t expectedPixel[4]) {
        ComPtr<ID3D11Resource> resource;
        ComPtr<ID3D11Texture2D> texture;
        if (!view)
            return false;
        view->GetResource(&resource);
        if (!resource || FAILED(resource.As(&texture)) || !texture)
            return false;
        context->CopyResource(staging.Get(), texture.Get());
        context->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(
                staging.Get(),
                0u,
                D3D11_MAP_READ,
                0u,
                &mapped))
            || !mapped.pData)
        {
            return false;
        }
        const auto* copied = static_cast<const uint8_t*>(mapped.pData);
        const bool match = copied[0] == expectedPixel[0]
            && copied[1] == expectedPixel[1]
            && copied[2] == expectedPixel[2]
            && copied[3] == expectedPixel[3];
        context->Unmap(staging.Get(), 0u);
        return match;
    };
    const auto viewsUseDifferentTextures = [](
        ID3D11ShaderResourceView* left,
        ID3D11ShaderResourceView* right) {
        ComPtr<ID3D11Resource> leftResource;
        ComPtr<ID3D11Resource> rightResource;
        if (!left || !right)
            return false;
        left->GetResource(&leftResource);
        right->GetResource(&rightResource);
        return leftResource && rightResource
            && leftResource.Get() != rightResource.Get();
    };
    if (!viewsUseDifferentTextures(frame.eyeViews[0], frame.eyeViews[1])
        || !pixelsMatch(frame.eyeViews[0], expected)
        || !pixelsMatch(frame.eyeViews[1], expected))
    {
        cleanup();
        failure = "CPU mailbox self-test mono texture duplication mismatch";
        return false;
    }

    // Exercise a true two-image world transaction in the same resource set.
    const uint8_t leftExpected[] = { 0x11u, 0x22u, 0x33u, 0xffu };
    const uint8_t rightExpected[] = { 0x77u, 0x88u, 0x99u, 0xffu };
    auto* stereoSlotPixels = pixels
        + static_cast<size_t>(gtaiv_xr_bridge::CpuFrameSlotBytes);
    std::memcpy(stereoSlotPixels, leftExpected, sizeof(leftExpected));
    std::memcpy(
        stereoSlotPixels + gtaiv_xr_bridge::CpuFrameEyeBytes,
        rightExpected,
        sizeof(rightExpected));
    mailbox->slotTransactionId[1] = 2u;
    MemoryBarrier();
    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&mailbox->slotSequence[1]),
        2);

    FrameBridge stereoValue = value;
    stereoValue.publicationSequence = 4;
    stereoValue.currentSlot = 1u;
    stereoValue.transactionId = 2u;
    stereoValue.sourceFrameId[0] = 2u;
    stereoValue.sourceFrameId[1] = 2u;
    stereoValue.poseSequence[0] = 2u;
    stereoValue.poseSequence[1] = 2u;
    stereoValue.renderedDisplayTime[0] = 2;
    stereoValue.renderedDisplayTime[1] = 2;
    stereoValue.heartbeatTickMs = GetTickCount64();
    stereoValue.slots[1].transactionId = 2u;
    stereoValue.flags = gtaiv_xr_bridge::Running
        | gtaiv_xr_bridge::CpuBgraMailbox
        | gtaiv_xr_bridge::Stereo
        | gtaiv_xr_bridge::EyesDistinct
        | gtaiv_xr_bridge::PoseStamped
        | gtaiv_xr_bridge::SameSimulationTick
        | gtaiv_xr_bridge::VerifiedDrawSceneStereo
        | gtaiv_xr_bridge::VerifiedParentDualStereo
        | gtaiv_xr_bridge::FirstPersonCamera
        | gtaiv_xr_bridge::NativeHeadHidden;
    stereoValue.presentationMode =
        gtaiv_xr_bridge::PresentationMode::WorldStereo;
    stereoValue.uiReasonFlags = gtaiv_xr_bridge::UiReasonNone;
    stereoValue.uiEye = 0u;
    std::memcpy(descriptor, &stereoValue, sizeof(stereoValue));
    MemoryBarrier();

    const GameFrameView stereoFrame = bridge.update();
    if (!stereoFrame
        || stereoFrame.transactionId != 2u
        || stereoFrame.presentationMode
            != gtaiv_xr_bridge::PresentationMode::WorldStereo
        || !stereoFrame.sameSimulationTick
        || stereoFrame.temporalStereo
        || !stereoFrame.parentDualStereo
        || !stereoFrame.firstPersonCamera
        || !stereoFrame.nativeHeadHidden
        || !stereoFrame.pixelDistinct
        || !stereoFrame.requiresExactCapturePose()
        || !viewsUseDifferentTextures(
            stereoFrame.eyeViews[0], stereoFrame.eyeViews[1])
        || !pixelsMatch(stereoFrame.eyeViews[0], leftExpected)
        || !pixelsMatch(stereoFrame.eyeViews[1], rightExpected))
    {
        cleanup();
        failure = "CPU mailbox self-test distinct L/R upload mismatch";
        return false;
    }

    // Exercise the same resource set across world stereo -> stationary menu.
    const uint8_t uiExpected[] = { 0xabu, 0xcdu, 0xefu, 0xffu };
    auto* uiSlotPixels = pixels
        + 2u * static_cast<size_t>(gtaiv_xr_bridge::CpuFrameSlotBytes);
    std::memcpy(uiSlotPixels, uiExpected, sizeof(uiExpected));
    mailbox->slotTransactionId[2] = 3u;
    MemoryBarrier();
    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&mailbox->slotSequence[2]),
        2);

    FrameBridge uiValue = stereoValue;
    uiValue.publicationSequence = 6;
    uiValue.currentSlot = 2u;
    uiValue.transactionId = 3u;
    uiValue.sourceFrameId[0] = 3u;
    uiValue.sourceFrameId[1] = 3u;
    uiValue.poseSequence[0] = 3u;
    uiValue.poseSequence[1] = 3u;
    uiValue.renderedDisplayTime[0] = 3;
    uiValue.renderedDisplayTime[1] = 3;
    uiValue.heartbeatTickMs = GetTickCount64();
    uiValue.slots[2].transactionId = 3u;
    uiValue.flags = gtaiv_xr_bridge::Running
        | gtaiv_xr_bridge::CpuBgraMailbox
        | gtaiv_xr_bridge::PoseStamped
        | gtaiv_xr_bridge::SameSimulationTick;
    uiValue.presentationMode = gtaiv_xr_bridge::PresentationMode::UiQuad;
    uiValue.uiReasonFlags = gtaiv_xr_bridge::UiReasonPauseOrMap;
    uiValue.uiEye = 0u;
    std::memcpy(descriptor, &uiValue, sizeof(uiValue));
    MemoryBarrier();

    const GameFrameView uiFrame = bridge.update();
    const bool uiPixelsMatch = uiFrame
        && pixelsMatch(uiFrame.eyeViews[0], uiExpected)
        && pixelsMatch(uiFrame.eyeViews[1], uiExpected);
    if (!uiFrame
        || uiFrame.transactionId != 3u
        || uiFrame.presentationMode
            != gtaiv_xr_bridge::PresentationMode::UiQuad
        || uiFrame.uiReasonFlags
            != gtaiv_xr_bridge::UiReasonPauseOrMap
        || !uiPixelsMatch)
    {
        cleanup();
        failure = "CPU mailbox self-test did not switch world stereo to UI";
        return false;
    }
    cleanup();
    failure.clear();
    return true;
}
}
