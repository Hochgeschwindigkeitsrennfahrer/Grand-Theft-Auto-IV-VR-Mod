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
            return DXGI_FORMAT_B8G8R8X8_UNORM;
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
    if ((first & 1) != 0)
        return false;
    MemoryBarrier();
    std::memcpy(&output, shared, sizeof(output));
    MemoryBarrier();
    const int32_t second = shared->publicationSequence;
    return first == second && (second & 1) == 0;
}
}

struct GameBridge::Implementation
{
    explicit Implementation(LogFunction function)
        : logger(std::move(function))
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

    bool initialize(ID3D11Device* sourceDevice, ID3D11DeviceContext* sourceContext)
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
        log("GameBridge: waiting for GTAIV.exe GPU frames");
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
        privateView.Reset();
        privateTexture.Reset();
        releaseFence.Reset();
        readyFence.Reset();
        for (ComPtr<ID3D11Texture2D>& texture : sharedTextures)
            texture.Reset();
        if (producerProcess)
        {
            CloseHandle(producerProcess);
            producerProcess = nullptr;
        }
        connectedPid = 0u;
        connectedEpoch = 0u;
        connectedGeneration = 0u;
        lastFrameId = 0u;
        privateWidth = 0u;
        privateHeight = 0u;
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
                "GameBridge: GTA frame bridge is not running yet",
                lastMappingLogTick,
                5000u);
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
                "GameBridge: could not map GTA frame descriptor",
                lastMappingLogTick,
                5000u);
            return false;
        }
        log("GameBridge: GTA frame descriptor found");
        return true;
    }

    void logRateLimited(
        const char* message,
        uint64_t& lastTick,
        uint64_t interval)
    {
        const uint64_t now = GetTickCount64();
        if (lastTick == 0u || now - lastTick >= interval)
        {
            log(message);
            lastTick = now;
        }
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

    bool duplicateHandle(
        uint64_t sourceValue,
        HANDLE& destination)
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
                & (D3D11_RESOURCE_MISC_SHARED_NTHANDLE
                    | D3D11_RESOURCE_MISC_SHARED))
                == (D3D11_RESOURCE_MISC_SHARED_NTHANDLE
                    | D3D11_RESOURCE_MISC_SHARED)
            && (description.MiscFlags
                & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) == 0u;
    }

    bool createPrivateTexture(const FrameBridge& value)
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

        if (FAILED(device->CreateTexture2D(
                &description,
                nullptr,
                &privateTexture))
            || !privateTexture
            || FAILED(device->CreateShaderResourceView(
                privateTexture.Get(),
                nullptr,
                &privateView))
            || !privateView)
        {
            return false;
        }
        privateWidth = value.width;
        privateHeight = value.height;
        return true;
    }

    bool openResourceSet(const FrameBridge& value)
    {
        releaseResources();
        if (value.adapterLuid != adapterLuid
            || value.width == 0u
            || value.height == 0u
            || dxgiFormat(value.format) == DXGI_FORMAT_UNKNOWN
            || !rawHandleValid(value.readyFenceHandle)
            || !rawHandleValid(value.releaseFenceHandle))
        {
            std::ostringstream message;
            message << "GameBridge: producer descriptor rejected; hostLuid="
                    << std::hex << adapterLuid
                    << " producerLuid=" << value.adapterLuid;
            logRateLimited(
                message.str().c_str(),
                lastOpenFailureLogTick,
                5000u);
            return false;
        }
        for (const gtaiv_xr_bridge::FrameSlot& slot : value.slots)
        {
            if (!rawHandleValid(slot.textureHandle))
                return false;
        }

        constexpr DWORD access =
            PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
        producerProcess = OpenProcess(access, FALSE, value.producerPid);
        if (!producerProcess || !producerActive(value.producerPid))
        {
            logRateLimited(
                "GameBridge: GTA producer process is unavailable",
                lastOpenFailureLogTick,
                5000u);
            releaseResources();
            return false;
        }

        std::array<HANDLE, gtaiv_xr_bridge::SlotCount> textureHandles {};
        HANDLE readyHandle = nullptr;
        HANDLE releaseHandle = nullptr;
        bool duplicated = true;
        for (uint32_t slot = 0u; slot < gtaiv_xr_bridge::SlotCount; ++slot)
        {
            duplicated = duplicated
                && duplicateHandle(
                    value.slots[slot].textureHandle,
                    textureHandles[slot]);
        }
        duplicated = duplicated
            && duplicateHandle(value.readyFenceHandle, readyHandle)
            && duplicateHandle(value.releaseFenceHandle, releaseHandle);

        bool opened = duplicated;
        if (opened)
        {
            for (uint32_t slot = 0u;
                 slot < gtaiv_xr_bridge::SlotCount;
                 ++slot)
            {
                if (FAILED(device5->OpenSharedResource1(
                        textureHandles[slot],
                        IID_PPV_ARGS(&sharedTextures[slot])))
                    || !sharedTextures[slot])
                {
                    opened = false;
                    break;
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

        for (HANDLE handle : textureHandles)
        {
            if (handle)
                CloseHandle(handle);
        }
        if (readyHandle)
            CloseHandle(readyHandle);
        if (releaseHandle)
            CloseHandle(releaseHandle);

        if (!opened)
        {
            logRateLimited(
                "GameBridge: opening GTA shared GPU resources failed",
                lastOpenFailureLogTick,
                5000u);
            releaseResources();
            return false;
        }
        for (const ComPtr<ID3D11Texture2D>& texture : sharedTextures)
        {
            if (!textureDescriptionMatches(texture.Get(), value))
            {
                logRateLimited(
                    "GameBridge: GTA shared texture description mismatch",
                    lastOpenFailureLogTick,
                    5000u);
                releaseResources();
                return false;
            }
        }
        if (!createPrivateTexture(value))
        {
            logRateLimited(
                "GameBridge: private host texture creation failed",
                lastOpenFailureLogTick,
                5000u);
            releaseResources();
            return false;
        }

        connectedPid = value.producerPid;
        connectedEpoch = value.producerEpoch;
        connectedGeneration = value.resourceGeneration;
        std::ostringstream message;
        message << "GameBridge: CONNECTED to GTAIV.exe pid=" << connectedPid
                << " source=" << value.width << 'x' << value.height
                << " adapterLuid=" << std::hex << value.adapterLuid;
        log(message.str());
        return true;
    }

    bool descriptorValid(const FrameBridge& value) const
    {
        return value.magic == gtaiv_xr_bridge::Magic
            && value.version == gtaiv_xr_bridge::Version
            && value.structBytes == sizeof(FrameBridge)
            && value.slotCount == gtaiv_xr_bridge::SlotCount
            && value.producerPid != 0u
            && value.producerEpoch != 0u
            && (value.flags & gtaiv_xr_bridge::Running) != 0u;
    }

    GameFrameView currentView() const
    {
        return {
            privateView.Get(),
            privateWidth,
            privateHeight,
            lastFrameId,
        };
    }

    GameFrameView update()
    {
        if (!ensureMapping())
            return currentView();

        FrameBridge value {};
        if (!stableSnapshot(shared, value))
            return currentView();
        if (!descriptorValid(value))
        {
            if (connectedPid != 0u)
            {
                log("GameBridge: GTA producer stopped; waiting for GTAIV.exe");
                releaseResources();
            }
            return currentView();
        }

        const bool changed =
            connectedPid != value.producerPid
            || connectedEpoch != value.producerEpoch
            || connectedGeneration != value.resourceGeneration;
        if (changed && !openResourceSet(value))
            return currentView();
        if (!producerActive(value.producerPid))
        {
            log("GameBridge: GTAIV.exe exited; waiting for restart");
            releaseResources();
            releaseMapping();
            return currentView();
        }
        if (value.frameId == 0u || value.frameId == lastFrameId)
            return currentView();
        if (value.currentSlot >= gtaiv_xr_bridge::SlotCount)
            return currentView();

        const gtaiv_xr_bridge::FrameSlot& slot =
            value.slots[value.currentSlot];
        if (slot.readyValue == 0u)
            return currentView();

        // If this value was already released, the producer is allowed to
        // overwrite its slot. Wait for a fresh publication instead.
        if (releaseFence->GetCompletedValue() >= slot.readyValue)
            return currentView();

        HRESULT result = context4->Wait(readyFence.Get(), slot.readyValue);
        if (FAILED(result))
        {
            logRateLimited(
                "GameBridge: GPU wait for GTA frame failed",
                lastConsumeFailureLogTick,
                5000u);
            return currentView();
        }
        context->CopyResource(
            privateTexture.Get(),
            sharedTextures[value.currentSlot].Get());
        result = context4->Signal(releaseFence.Get(), slot.readyValue);
        if (FAILED(result))
        {
            logRateLimited(
                "GameBridge: GPU release signal failed",
                lastConsumeFailureLogTick,
                5000u);
            return currentView();
        }
        context->Flush();
        if (device->GetDeviceRemovedReason() != S_OK)
        {
            log("GameBridge: host D3D11 device was removed");
            releaseResources();
            return currentView();
        }

        lastFrameId = value.frameId;
        if (lastFrameId == 1u || lastFrameId % 120u == 0u)
        {
            std::ostringstream message;
            message << "GameBridge: GTA frame acquired frame=" << lastFrameId
                    << " slot=" << value.currentSlot;
            log(message.str());
        }
        return currentView();
    }

    LogFunction logger;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    uint64_t adapterLuid = 0u;

    HANDLE mapping = nullptr;
    const FrameBridge* shared = nullptr;
    HANDLE producerProcess = nullptr;
    std::array<ComPtr<ID3D11Texture2D>, gtaiv_xr_bridge::SlotCount>
        sharedTextures;
    ComPtr<ID3D11Fence> readyFence;
    ComPtr<ID3D11Fence> releaseFence;
    ComPtr<ID3D11Texture2D> privateTexture;
    ComPtr<ID3D11ShaderResourceView> privateView;

    uint32_t connectedPid = 0u;
    uint64_t connectedEpoch = 0u;
    uint32_t connectedGeneration = 0u;
    uint64_t lastFrameId = 0u;
    uint32_t privateWidth = 0u;
    uint32_t privateHeight = 0u;
    uint64_t lastMappingLogTick = 0u;
    uint64_t lastOpenFailureLogTick = 0u;
    uint64_t lastConsumeFailureLogTick = 0u;
};

GameBridge::GameBridge() = default;

GameBridge::~GameBridge()
{
    reset();
}

bool GameBridge::initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    LogFunction logger)
{
    reset();
    implementation_ = new (std::nothrow) Implementation(std::move(logger));
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
}
