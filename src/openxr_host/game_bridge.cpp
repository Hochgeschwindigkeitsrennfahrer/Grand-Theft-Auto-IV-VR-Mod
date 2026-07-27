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
    bool uiQuad = false;
    const char* rejection = nullptr;
};

TransactionQuality assessTransactionQuality(
    const FrameBridge& value,
    bool allowTemporalStereo)
{
    TransactionQuality result {};
    result.sameTick =
        (value.flags & gtaiv_xr_bridge::SameSimulationTick) != 0u
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
    result.uiQuad =
        value.presentationMode
            == gtaiv_xr_bridge::PresentationMode::UiQuad;
    constexpr uint32_t KnownUiReasons =
        gtaiv_xr_bridge::UiReasonPauseOrMap
        | gtaiv_xr_bridge::UiReasonLoading
        | gtaiv_xr_bridge::UiReasonPhone;
    const bool reasonsValid =
        (value.uiReasonFlags & ~KnownUiReasons) == 0u;
    if ((!worldStereo && !result.uiQuad)
        || value.uiEye >= gtaiv_xr_bridge::EyeCount
        || !reasonsValid
        || (worldStereo
            && value.uiReasonFlags != gtaiv_xr_bridge::UiReasonNone)
        || (result.uiQuad
            && value.uiReasonFlags == gtaiv_xr_bridge::UiReasonNone))
    {
        result.rejection = "invalid presentation mode";
        return result;
    }
    if (worldStereo && !result.sameTick && !allowTemporalStereo)
    {
        result.rejection = "not one simulation tick/pose";
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
            || !acquireAdapterLuid())
        {
            log("GameBridge: D3D11 fence interfaces unavailable");
            resetAll();
            return false;
        }
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

    void releaseResources()
    {
        for (ComPtr<ID3D11ShaderResourceView>& view : privateViews)
            view.Reset();
        for (ComPtr<ID3D11Texture2D>& texture : privateTextures)
            texture.Reset();
        for (auto& slot : sharedTextures)
        {
            for (ComPtr<ID3D11Texture2D>& texture : slot)
                texture.Reset();
        }
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
        lastTransaction = 0u;
        current = {};
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
        log("GameBridge: GTA stereo descriptor v3 found");
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

    bool createPrivateTextures(const FrameBridge& value)
    {
        D3D11_TEXTURE2D_DESC description {};
        description.Width = value.width;
        description.Height = value.height;
        description.MipLevels = 1u;
        description.ArraySize = 1u;
        description.Format = dxgiFormat(value.format);
        description.SampleDesc.Count = 1u;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (uint32_t eye = 0u;
             eye < gtaiv_xr_bridge::EyeCount;
             ++eye)
        {
            if (FAILED(device->CreateTexture2D(
                    &description,
                    nullptr,
                    &privateTextures[eye]))
                || !privateTextures[eye]
                || FAILED(device->CreateShaderResourceView(
                    privateTextures[eye].Get(),
                    nullptr,
                    &privateViews[eye]))
                || !privateViews[eye])
            {
                return false;
            }
        }
        return privateViews[0].Get() != privateViews[1].Get();
    }

    bool openResourceSet(const FrameBridge& value)
    {
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
                    if (FAILED(device5->OpenSharedResource1(
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
                SUCCEEDED(device5->OpenSharedFence(
                    readyHandle,
                    IID_PPV_ARGS(&readyFence)))
                && readyFence
                && SUCCEEDED(device5->OpenSharedFence(
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
        message << "GameBridge: CONNECTED stereo pid=" << connectedPid
                << " source=" << value.width << 'x' << value.height
                << " adapterLuid=" << std::hex << value.adapterLuid
                << " resourceSet=" << value.resourceSetId;
        log(message.str());
        return true;
    }

    bool descriptorHeaderValid(const FrameBridge& value) const
    {
        const uint64_t now = GetTickCount64();
        return value.magic == gtaiv_xr_bridge::Magic
            && value.version == gtaiv_xr_bridge::Version
            && value.structBytes == sizeof(FrameBridge)
            && value.mappingBytes == sizeof(FrameBridge)
            && value.slotCount == gtaiv_xr_bridge::SlotCount
            && value.producerPid != 0u
            && value.producerEpoch != 0u
            && value.resourceSetId != 0u
            && value.heartbeatTickMs != 0u
            && now >= value.heartbeatTickMs
            && now - value.heartbeatTickMs <= 1000u
            && (value.flags & gtaiv_xr_bridge::Running) != 0u;
    }

    bool drainRejectedTransaction(
        const FrameBridge& value,
        const char* reason)
    {
        if (value.currentSlot >= gtaiv_xr_bridge::SlotCount
            || value.transactionId == 0u
            || value.slots[value.currentSlot].transactionId
                != value.transactionId)
        {
            return false;
        }
        HRESULT result = context4->Wait(
            readyFence.Get(),
            value.transactionId);
        if (SUCCEEDED(result))
        {
            result = context4->Signal(
                releaseFence.Get(),
                value.transactionId);
            context->Flush();
        }
        std::ostringstream message;
        message << "GameBridge: transaction " << value.transactionId
                << " rejected (" << reason << ')';
        logRateLimited(message.str(), lastQualityRejectLogTick, 2000u);
        if (SUCCEEDED(result))
        {
            lastTransaction = value.transactionId;
            current = {};
        }
        return SUCCEEDED(result);
    }

    GameFrameView update()
    {
        if (!ensureMapping())
            return current;

        FrameBridge value {};
        if (!stableSnapshot(shared, value))
            return current;
        if (!descriptorHeaderValid(value))
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
        if (value.transactionId == 0u
            || value.transactionId == lastTransaction)
        {
            return current;
        }
        if (value.transactionId < lastTransaction
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

        HRESULT result = context4->Wait(
            readyFence.Get(),
            value.transactionId);
        if (FAILED(result))
        {
            logRateLimited(
                "GameBridge: GPU wait for stereo transaction failed",
                lastConsumeFailureLogTick);
            return current;
        }
        for (uint32_t eye = 0u;
             eye < gtaiv_xr_bridge::EyeCount;
             ++eye)
        {
            context->CopyResource(
                privateTextures[eye].Get(),
                sharedTextures[value.currentSlot][eye].Get());
        }
        result = context4->Signal(
            releaseFence.Get(),
            value.transactionId);
        if (FAILED(result))
        {
            logRateLimited(
                "GameBridge: GPU release signal failed",
                lastConsumeFailureLogTick);
            return current;
        }
        context->Flush();
        if (device->GetDeviceRemovedReason() != S_OK)
        {
            log("GameBridge: host D3D11 device was removed");
            releaseResources();
            return current;
        }

        lastTransaction = value.transactionId;
        current = {};
        current.eyeViews[0] = privateViews[0].Get();
        current.eyeViews[1] = privateViews[1].Get();
        current.width = value.width;
        current.height = value.height;
        current.transactionId = value.transactionId;
        current.sameSimulationTick = quality.sameTick;
        current.presentationMode = value.presentationMode;
        current.uiReasonFlags = value.uiReasonFlags;
        current.uiEye = value.uiEye;
        for (uint32_t eye = 0u;
             eye < gtaiv_xr_bridge::EyeCount;
             ++eye)
        {
            current.sourceFrameId[eye] = value.sourceFrameId[eye];
            current.poseSequence[eye] = value.poseSequence[eye];
            current.renderedDisplayTime[eye] =
                value.renderedDisplayTime[eye];
        }
        if (lastTransaction == 1u || lastTransaction % 120u == 0u)
        {
            std::ostringstream message;
            message << "GameBridge: stereo acquired transaction="
                    << lastTransaction
                    << " slot=" << value.currentSlot
                    << " sameTick=" << (quality.sameTick ? 1 : 0)
                    << " presentation="
                    << (quality.uiQuad ? "ui-quad" : "world-stereo")
                    << " pose=" << value.poseSequence[0]
                    << '/' << value.poseSequence[1];
            log(message.str());
        }
        return current;
    }

    LogFunction logger;
    const bool allowTemporalStereo = false;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    uint64_t adapterLuid = 0u;

    HANDLE mapping = nullptr;
    const FrameBridge* shared = nullptr;
    HANDLE producerProcess = nullptr;
    SharedTextureSet sharedTextures;
    ComPtr<ID3D11Fence> readyFence;
    ComPtr<ID3D11Fence> releaseFence;
    std::array<
        ComPtr<ID3D11Texture2D>,
        gtaiv_xr_bridge::EyeCount> privateTextures;
    std::array<
        ComPtr<ID3D11ShaderResourceView>,
        gtaiv_xr_bridge::EyeCount> privateViews;
    GameFrameView current {};

    uint32_t connectedPid = 0u;
    uint64_t connectedEpoch = 0u;
    uint64_t connectedResourceSet = 0u;
    uint32_t connectedGeneration = 0u;
    uint64_t lastTransaction = 0u;
    uint64_t lastMappingLogTick = 0u;
    uint64_t lastOpenFailureLogTick = 0u;
    uint64_t lastConsumeFailureLogTick = 0u;
    uint64_t lastQualityRejectLogTick = 0u;
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
        | gtaiv_xr_bridge::SameSimulationTick;
    value.presentationMode =
        gtaiv_xr_bridge::PresentationMode::WorldStereo;
    value.sourceFrameId[0] = 10u;
    value.sourceFrameId[1] = 10u;
    value.poseSequence[0] = 20u;
    value.poseSequence[1] = 20u;
    value.renderedDisplayTime[0] = 30;
    value.renderedDisplayTime[1] = 30;
    value.uiEye = 0u;
    if (!assessTransactionQuality(value, false).accepted)
    {
        failure = "strict same-tick world pair was rejected";
        return false;
    }

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

    value.presentationMode =
        gtaiv_xr_bridge::PresentationMode::UiQuad;
    value.uiReasonFlags = gtaiv_xr_bridge::UiReasonPauseOrMap;
    value.uiEye = 1u;
    const TransactionQuality uiQuality =
        assessTransactionQuality(value, false);
    if (!uiQuality.accepted || !uiQuality.uiQuad)
    {
        failure = "valid temporal UI quad transaction was rejected";
        return false;
    }

    value.uiReasonFlags = gtaiv_xr_bridge::UiReasonNone;
    if (assessTransactionQuality(value, false).accepted)
    {
        failure = "reasonless UI quad transaction was accepted";
        return false;
    }
    failure.clear();
    return true;
}
}
