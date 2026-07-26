#include "openxr_bridge.h"

#include "log.h"
#include "../bridge/gtaiv_xr_frame_bridge.h"

#include <windows.h>

#include <d3d9.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace asi
{
namespace
{
using Microsoft::WRL::ComPtr;
using gtaiv_xr_bridge::FrameBridge;
using gtaiv_xr_bridge::PixelFormat;

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

PixelFormat protocolFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return PixelFormat::B8G8R8A8Unorm;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            return PixelFormat::B8G8R8X8Unorm;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return PixelFormat::R8G8B8A8Unorm;
        default:
            return PixelFormat::Unknown;
    }
}

bool sameDevice(IUnknown* left, IUnknown* right)
{
    if (!left || !right)
        return false;
    ComPtr<IUnknown> leftIdentity;
    ComPtr<IUnknown> rightIdentity;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&leftIdentity)))
        && SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&rightIdentity)))
        && leftIdentity.Get() == rightIdentity.Get();
}

class OpenXrFrameProducer
{
public:
    ~OpenXrFrameProducer()
    {
        shutdown();
    }

    void publish(IDirect3DDevice9* gameDevice)
    {
        if (!gameDevice)
            return;

        ComPtr<IDirect3DSurface9> backBuffer;
        HRESULT result = gameDevice->GetBackBuffer(
            0u,
            0u,
            D3DBACKBUFFER_TYPE_MONO,
            &backBuffer);
        if (FAILED(result) || !backBuffer)
        {
            logFailureOnce("GetBackBuffer", result);
            return;
        }

        D3DSURFACE_DESC description {};
        result = backBuffer->GetDesc(&description);
        if (FAILED(result)
            || description.Width == 0u
            || description.Height == 0u)
        {
            logFailureOnce("GetBackBuffer description", result);
            return;
        }

        if (!ready_
            || !sameDevice(gameDevice_.Get(), gameDevice)
            || width_ != description.Width
            || height_ != description.Height
            || d3d9Format_ != description.Format)
        {
            if (ready_)
            {
                Log(
                    "OpenXRBridge: source changed; rebuilding %ux%u format=%u",
                    description.Width,
                    description.Height,
                    static_cast<unsigned>(description.Format));
            }
            shutdown();
            if (!initialize(gameDevice, description))
                return;
        }

        publishHeartbeat();

        const uint64_t completedRelease = releaseFence_->GetCompletedValue();
        uint32_t selectedSlot = gtaiv_xr_bridge::SlotCount;
        for (uint32_t offset = 0u; offset < gtaiv_xr_bridge::SlotCount; ++offset)
        {
            const uint32_t slot = (nextSlot_ + offset) % gtaiv_xr_bridge::SlotCount;
            if (slotReadyValues_[slot] == 0u
                || completedRelease >= slotReadyValues_[slot])
            {
                selectedSlot = slot;
                break;
            }
        }
        if (selectedSlot == gtaiv_xr_bridge::SlotCount)
        {
            const uint64_t now = GetTickCount64();
            if (now - lastBusyLogTick_ >= 5000u)
            {
                Log(
                    "OpenXRBridge: waiting for x64 host; newest GTA frame remains published");
                lastBusyLogTick_ = now;
            }
            return;
        }

        result = gameDevice->StretchRect(
            backBuffer.Get(),
            nullptr,
            d3d9SharedSurface_.Get(),
            nullptr,
            D3DTEXF_NONE);
        if (FAILED(result))
        {
            logFailureOnce("StretchRect(backbuffer -> shared D3D9 surface)", result);
            return;
        }

        if (!waitForD3D9Writes())
            return;

        d3d11Context_->CopyResource(
            sharedSlots_[selectedSlot].Get(),
            d3d11Source_.Get());

        const uint64_t readyValue = ++nextReadyValue_;
        result = d3d11Context4_->Signal(readyFence_.Get(), readyValue);
        if (FAILED(result))
        {
            logFailureOnce("ID3D11DeviceContext4::Signal(ready)", result);
            return;
        }
        d3d11Context_->Flush();
        if (d3d11Device_->GetDeviceRemovedReason() != S_OK)
        {
            logFailureOnce(
                "D3D11 bridge device removed",
                d3d11Device_->GetDeviceRemovedReason());
            return;
        }

        slotReadyValues_[selectedSlot] = readyValue;
        currentSlot_ = selectedSlot;
        nextSlot_ = (selectedSlot + 1u) % gtaiv_xr_bridge::SlotCount;
        frameId_ = readyValue;
        publishDescriptor();

        if (frameId_ == 1u || frameId_ % 120u == 0u)
        {
            Log(
                "OpenXRBridge: GTA frame published frame=%llu slot=%u size=%ux%u",
                static_cast<unsigned long long>(frameId_),
                currentSlot_,
                width_,
                height_);
        }
        lastFailureOperation_.clear();
    }

    void shutdown()
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
            stopped.heartbeatTickMs = GetTickCount64();
            stopped.resourceGeneration = resourceGeneration_;
            commit(stopped);
        }

        ready_ = false;
        if (shared_)
        {
            UnmapViewOfFile(shared_);
            shared_ = nullptr;
        }
        if (mapping_)
        {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        for (HANDLE& handle : slotHandles_)
        {
            if (handle)
                CloseHandle(handle);
            handle = nullptr;
        }
        if (readyFenceHandle_)
        {
            CloseHandle(readyFenceHandle_);
            readyFenceHandle_ = nullptr;
        }
        if (releaseFenceHandle_)
        {
            CloseHandle(releaseFenceHandle_);
            releaseFenceHandle_ = nullptr;
        }
        for (ComPtr<ID3D11Texture2D>& texture : sharedSlots_)
            texture.Reset();
        releaseFence_.Reset();
        readyFence_.Reset();
        d3d11Source_.Reset();
        d3d11Context4_.Reset();
        d3d11Context_.Reset();
        d3d11Device5_.Reset();
        d3d11Device_.Reset();
        dxgiAdapter_.Reset();
        d3d9CompletionQuery_.Reset();
        d3d9SharedSurface_.Reset();
        gameDevice_.Reset();

        slotReadyValues_.fill(0u);
        width_ = 0u;
        height_ = 0u;
        d3d9Format_ = D3DFMT_UNKNOWN;
        dxgiFormat_ = DXGI_FORMAT_UNKNOWN;
        currentSlot_ = 0u;
        nextSlot_ = 0u;
        nextReadyValue_ = 0u;
        frameId_ = 0u;
    }

private:
    bool initialize(
        IDirect3DDevice9* gameDevice,
        const D3DSURFACE_DESC& backBufferDescription)
    {
        if (producerEpoch_ == 0u)
        {
            LARGE_INTEGER counter {};
            QueryPerformanceCounter(&counter);
            producerEpoch_ =
                static_cast<uint64_t>(counter.QuadPart)
                ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32u)
                ^ GetTickCount64();
            if (producerEpoch_ == 0u)
                producerEpoch_ = 1u;
        }
        ++resourceGeneration_;
        if (resourceGeneration_ == 0u)
            ++resourceGeneration_;

        gameDevice_ = gameDevice;
        width_ = backBufferDescription.Width;
        height_ = backBufferDescription.Height;
        d3d9Format_ = backBufferDescription.Format;

        if (!createMatchingD3D11Device(gameDevice))
            return failInitialization("matching native D3D11 device");
        if (!createD3D9SharedSource(gameDevice))
            return failInitialization("DXVK D3D9 shared render target");
        if (!openD3D9Source())
            return failInitialization("native D3D11 open of DXVK shared texture");
        if (!createSharedSlotsAndFences())
            return failInitialization("cross-process D3D11 textures/fences");
        if (!createMapping())
            return failInitialization("shared frame descriptor");

        ready_ = true;
        publishDescriptor();
        Log(
            "OpenXRBridge: READY producer=x86 host=x64 adapterLuid=%08lx%08lx "
            "source=%ux%u dxgiFormat=%u slots=%u",
            static_cast<unsigned long>(
                static_cast<uint32_t>(adapterLuid_ >> 32u)),
            static_cast<unsigned long>(
                static_cast<uint32_t>(adapterLuid_)),
            width_,
            height_,
            static_cast<unsigned>(dxgiFormat_),
            gtaiv_xr_bridge::SlotCount);
        return true;
    }

    bool failInitialization(const char* stage)
    {
        Log("OpenXRBridge: FAILED initializing %s", stage);
        shutdown();
        return false;
    }

    bool createMatchingD3D11Device(IDirect3DDevice9* gameDevice)
    {
        D3DDEVICE_CREATION_PARAMETERS creation {};
        ComPtr<IDirect3D9> direct3D;
        D3DADAPTER_IDENTIFIER9 identifier {};
        HMONITOR gameMonitor = nullptr;
        if (FAILED(gameDevice->GetCreationParameters(&creation))
            || FAILED(gameDevice->GetDirect3D(&direct3D))
            || !direct3D
            || FAILED(direct3D->GetAdapterIdentifier(
                creation.AdapterOrdinal,
                0u,
                &identifier)))
        {
            Log("OpenXRBridge: cannot identify GTA D3D9 adapter");
            return false;
        }
        gameMonitor = direct3D->GetAdapterMonitor(creation.AdapterOrdinal);

        ComPtr<IDXGIFactory1> factory;
        HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(result) || !factory)
        {
            Log(
                "OpenXRBridge: CreateDXGIFactory1 failed hr=0x%08lx",
                static_cast<unsigned long>(result));
            return false;
        }

        ComPtr<IDXGIAdapter1> vendorMatch;
        for (UINT adapterIndex = 0u;; ++adapterIndex)
        {
            ComPtr<IDXGIAdapter1> candidate;
            result = factory->EnumAdapters1(adapterIndex, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(result) || !candidate)
                return false;

            DXGI_ADAPTER_DESC1 description {};
            if (FAILED(candidate->GetDesc1(&description))
                || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0u)
            {
                continue;
            }

            bool monitorMatch = false;
            if (gameMonitor)
            {
                for (UINT outputIndex = 0u;; ++outputIndex)
                {
                    ComPtr<IDXGIOutput> output;
                    const HRESULT outputResult =
                        candidate->EnumOutputs(outputIndex, &output);
                    if (outputResult == DXGI_ERROR_NOT_FOUND)
                        break;
                    if (FAILED(outputResult) || !output)
                        break;
                    DXGI_OUTPUT_DESC outputDescription {};
                    if (SUCCEEDED(output->GetDesc(&outputDescription))
                        && outputDescription.Monitor == gameMonitor)
                    {
                        monitorMatch = true;
                        break;
                    }
                }
            }

            if (monitorMatch)
            {
                dxgiAdapter_ = candidate;
                break;
            }
            if (!vendorMatch
                && description.VendorId == identifier.VendorId
                && description.DeviceId == identifier.DeviceId)
            {
                vendorMatch = candidate;
            }
        }
        if (!dxgiAdapter_)
            dxgiAdapter_ = vendorMatch;
        if (!dxgiAdapter_)
        {
            Log(
                "OpenXRBridge: no DXGI adapter matched vendor=%04lx device=%04lx",
                static_cast<unsigned long>(identifier.VendorId),
                static_cast<unsigned long>(identifier.DeviceId));
            return false;
        }

        DXGI_ADAPTER_DESC1 adapterDescription {};
        if (FAILED(dxgiAdapter_->GetDesc1(&adapterDescription)))
            return false;
        adapterLuid_ = packLuid(adapterDescription.AdapterLuid);

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_10_0;
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
            || FAILED(d3d11Device_.As(&d3d11Device5_))
            || FAILED(d3d11Context_.As(&d3d11Context4_)))
        {
            Log(
                "OpenXRBridge: D3D11 fence-capable device failed hr=0x%08lx",
                static_cast<unsigned long>(result));
            return false;
        }
        return true;
    }

    bool createD3D9SharedSource(IDirect3DDevice9* gameDevice)
    {
        HANDLE sharedHandle = nullptr;
        HRESULT result = gameDevice->CreateRenderTarget(
            width_,
            height_,
            d3d9Format_,
            D3DMULTISAMPLE_NONE,
            0u,
            FALSE,
            &d3d9SharedSurface_,
            &sharedHandle);
        if (FAILED(result) || !d3d9SharedSurface_ || !sharedHandle)
        {
            Log(
                "OpenXRBridge: CreateRenderTarget(shared) failed hr=0x%08lx "
                "format=%u handle=%p",
                static_cast<unsigned long>(result),
                static_cast<unsigned>(d3d9Format_),
                sharedHandle);
            return false;
        }
        d3d9SharedHandle_ = sharedHandle;

        result = gameDevice->CreateQuery(
            D3DQUERYTYPE_EVENT,
            &d3d9CompletionQuery_);
        if (FAILED(result) || !d3d9CompletionQuery_)
        {
            Log(
                "OpenXRBridge: CreateQuery(EVENT) failed hr=0x%08lx",
                static_cast<unsigned long>(result));
            return false;
        }
        return true;
    }

    bool openD3D9Source()
    {
        HRESULT result = d3d11Device_->OpenSharedResource(
            d3d9SharedHandle_,
            IID_PPV_ARGS(&d3d11Source_));
        if (FAILED(result) || !d3d11Source_)
        {
            Log(
                "OpenXRBridge: OpenSharedResource(DXVK KMT) failed hr=0x%08lx "
                "handle=%p",
                static_cast<unsigned long>(result),
                d3d9SharedHandle_);
            return false;
        }

        D3D11_TEXTURE2D_DESC description {};
        d3d11Source_->GetDesc(&description);
        dxgiFormat_ = description.Format;
        if (description.Width != width_
            || description.Height != height_
            || description.MipLevels != 1u
            || description.ArraySize != 1u
            || description.SampleDesc.Count != 1u
            || protocolFormat(description.Format) == PixelFormat::Unknown)
        {
            Log(
                "OpenXRBridge: opened DXVK texture description rejected "
                "size=%ux%u format=%u samples=%u",
                description.Width,
                description.Height,
                static_cast<unsigned>(description.Format),
                description.SampleDesc.Count);
            return false;
        }
        return true;
    }

    bool createNtTexture(ComPtr<ID3D11Texture2D>& texture, HANDLE& handle)
    {
        D3D11_TEXTURE2D_DESC description {};
        description.Width = width_;
        description.Height = height_;
        description.MipLevels = 1u;
        description.ArraySize = 1u;
        description.Format = dxgiFormat_;
        description.SampleDesc.Count = 1u;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags =
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        description.MiscFlags =
            D3D11_RESOURCE_MISC_SHARED_NTHANDLE
            | D3D11_RESOURCE_MISC_SHARED;

        HRESULT result = d3d11Device_->CreateTexture2D(
            &description,
            nullptr,
            &texture);
        if (FAILED(result) || !texture)
        {
            Log(
                "OpenXRBridge: CreateTexture2D(NT shared) failed hr=0x%08lx",
                static_cast<unsigned long>(result));
            return false;
        }

        ComPtr<IDXGIResource1> shareable;
        result = texture.As(&shareable);
        if (FAILED(result) || !shareable)
            return false;
        result = shareable->CreateSharedHandle(
            nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr,
            &handle);
        if (FAILED(result) || !handle)
        {
            Log(
                "OpenXRBridge: CreateSharedHandle(texture) failed hr=0x%08lx",
                static_cast<unsigned long>(result));
            return false;
        }
        return true;
    }

    bool createFence(
        ComPtr<ID3D11Fence>& fence,
        HANDLE& handle,
        const char* name)
    {
        HRESULT result = d3d11Device5_->CreateFence(
            0u,
            D3D11_FENCE_FLAG_SHARED,
            IID_PPV_ARGS(&fence));
        if (FAILED(result) || !fence)
        {
            Log(
                "OpenXRBridge: CreateFence(%s) failed hr=0x%08lx",
                name,
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
                name,
                static_cast<unsigned long>(result));
            return false;
        }
        return true;
    }

    bool createSharedSlotsAndFences()
    {
        UINT support = 0u;
        const UINT required =
            D3D11_FORMAT_SUPPORT_TEXTURE2D
            | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE
            | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
        if (FAILED(d3d11Device_->CheckFormatSupport(dxgiFormat_, &support))
            || (support & required) != required)
        {
            Log(
                "OpenXRBridge: format %u lacks shared render/sample support",
                static_cast<unsigned>(dxgiFormat_));
            return false;
        }

        for (uint32_t slot = 0u; slot < gtaiv_xr_bridge::SlotCount; ++slot)
        {
            if (!createNtTexture(sharedSlots_[slot], slotHandles_[slot]))
                return false;
        }
        return createFence(readyFence_, readyFenceHandle_, "ready")
            && createFence(releaseFence_, releaseFenceHandle_, "release");
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
                "OpenXRBridge: CreateFileMapping failed win32=%lu",
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
                "OpenXRBridge: MapViewOfFile failed win32=%lu",
                static_cast<unsigned long>(GetLastError()));
            return false;
        }
        return true;
    }

    bool waitForD3D9Writes()
    {
        HRESULT result = d3d9CompletionQuery_->Issue(D3DISSUE_END);
        if (FAILED(result))
        {
            logFailureOnce("D3D9 event query Issue", result);
            return false;
        }

        const uint64_t started = GetTickCount64();
        for (;;)
        {
            result = d3d9CompletionQuery_->GetData(
                nullptr,
                0u,
                D3DGETDATA_FLUSH);
            if (result == S_OK)
                return true;
            if (result != S_FALSE || GetTickCount64() - started >= 250u)
            {
                logFailureOnce("D3D9/D3D11 bridge synchronization", result);
                return false;
            }
            SwitchToThread();
        }
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
        value.adapterLuid = adapterLuid_;
        value.width = width_;
        value.height = height_;
        value.format = protocolFormat(dxgiFormat_);
        value.currentSlot = currentSlot_;
        value.frameId = frameId_;
        value.heartbeatTickMs = GetTickCount64();
        value.readyFenceHandle = handleValue(readyFenceHandle_);
        value.releaseFenceHandle = handleValue(releaseFenceHandle_);
        for (uint32_t slot = 0u; slot < gtaiv_xr_bridge::SlotCount; ++slot)
        {
            value.slots[slot].textureHandle = handleValue(slotHandles_[slot]);
            value.slots[slot].readyValue = slotReadyValues_[slot];
        }
        value.flags = gtaiv_xr_bridge::Running | gtaiv_xr_bridge::Mono;
        value.resourceGeneration = resourceGeneration_;
        return value;
    }

    void commit(const FrameBridge& value)
    {
        if (!shared_)
            return;

        volatile LONG* sequence =
            reinterpret_cast<volatile LONG*>(&shared_->publicationSequence);
        LONG current = InterlockedCompareExchange(sequence, 0, 0);
        LONG writing = (current & 1) ? current + 2 : current + 1;
        InterlockedExchange(sequence, writing);
        MemoryBarrier();

        constexpr size_t prefixBytes = offsetof(FrameBridge, publicationSequence);
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
        commit(descriptor());
        lastHeartbeatPublishTick_ = GetTickCount64();
    }

    void publishHeartbeat()
    {
        const uint64_t now = GetTickCount64();
        if (now - lastHeartbeatPublishTick_ >= 500u)
            publishDescriptor();
    }

    void logFailureOnce(const char* operation, HRESULT result)
    {
        const uint64_t now = GetTickCount64();
        if (lastFailureOperation_ != operation
            || now - lastFailureLogTick_ >= 5000u)
        {
            Log(
                "OpenXRBridge: %s failed hr=0x%08lx",
                operation,
                static_cast<unsigned long>(result));
            lastFailureOperation_ = operation;
            lastFailureLogTick_ = now;
        }
    }

    bool ready_ = false;
    ComPtr<IDirect3DDevice9> gameDevice_;
    ComPtr<IDirect3DSurface9> d3d9SharedSurface_;
    ComPtr<IDirect3DQuery9> d3d9CompletionQuery_;
    HANDLE d3d9SharedHandle_ = nullptr;  // Legacy KMT handle; never CloseHandle.

    ComPtr<IDXGIAdapter1> dxgiAdapter_;
    ComPtr<ID3D11Device> d3d11Device_;
    ComPtr<ID3D11Device5> d3d11Device5_;
    ComPtr<ID3D11DeviceContext> d3d11Context_;
    ComPtr<ID3D11DeviceContext4> d3d11Context4_;
    ComPtr<ID3D11Texture2D> d3d11Source_;
    std::array<ComPtr<ID3D11Texture2D>, gtaiv_xr_bridge::SlotCount> sharedSlots_;
    std::array<HANDLE, gtaiv_xr_bridge::SlotCount> slotHandles_ {};
    ComPtr<ID3D11Fence> readyFence_;
    ComPtr<ID3D11Fence> releaseFence_;
    HANDLE readyFenceHandle_ = nullptr;
    HANDLE releaseFenceHandle_ = nullptr;

    HANDLE mapping_ = nullptr;
    FrameBridge* shared_ = nullptr;
    std::array<uint64_t, gtaiv_xr_bridge::SlotCount> slotReadyValues_ {};
    uint64_t producerEpoch_ = 0u;
    uint64_t adapterLuid_ = 0u;
    uint64_t nextReadyValue_ = 0u;
    uint64_t frameId_ = 0u;
    uint64_t lastHeartbeatPublishTick_ = 0u;
    uint64_t lastBusyLogTick_ = 0u;
    uint64_t lastFailureLogTick_ = 0u;
    uint32_t resourceGeneration_ = 0u;
    uint32_t currentSlot_ = 0u;
    uint32_t nextSlot_ = 0u;
    UINT width_ = 0u;
    UINT height_ = 0u;
    D3DFORMAT d3d9Format_ = D3DFMT_UNKNOWN;
    DXGI_FORMAT dxgiFormat_ = DXGI_FORMAT_UNKNOWN;
    std::string lastFailureOperation_;
};

OpenXrFrameProducer g_producer;
}

bool IsOpenXrBridgeRequested()
{
    static const bool requested = [] {
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
        const bool enabled = value.find("openxr") != std::string::npos;
        Log(
            "Backend: %s (gtaiv_dxvk_vr.backend=%s)",
            enabled ? "OpenXR x64 bridge" : "OpenVR in-process fallback",
            value.empty() ? "(missing/default)" : value.c_str());
        return enabled;
    }();
    return requested;
}

void PublishOpenXrFrame(IDirect3DDevice9* device)
{
    g_producer.publish(device);
}

void ShutdownOpenXrBridge()
{
    g_producer.shutdown();
}
}
