#pragma once

#include <cstddef>
#include <cstdint>

// Pointer-free x86 <-> x64 contract for one complete stereo GPU transaction.
// Handles are values in the producer process and must be duplicated by the
// consumer from producerPid before they are opened.
namespace gtaiv_xr_bridge
{
constexpr wchar_t MappingName[] = L"Local\\GTAIV_XR_FrameBridge_v3";
constexpr uint32_t Magic = 0x46525847u;  // "GXRF" in little-endian memory.
constexpr uint32_t Version = 3u;
constexpr uint32_t EyeCount = 2u;
constexpr uint32_t SlotCount = 3u;

enum class PixelFormat : uint32_t
{
    Unknown = 0u,
    B8G8R8A8Unorm = 1u,
    B8G8R8X8Unorm = 2u,
    R8G8B8A8Unorm = 3u,
};

enum FrameFlags : uint32_t
{
    Running = 1u << 0u,
    GpuD3D11NtHandles = 1u << 1u,
    Stereo = 1u << 2u,
    EyesDistinct = 1u << 3u,
    SameSimulationTick = 1u << 4u,
    PoseStamped = 1u << 5u,
    TemporalStereo = 1u << 6u,
};

enum class PresentationMode : uint32_t
{
    Unknown = 0u,
    WorldStereo = 1u,
    UiQuad = 2u,
};

enum UiReasonFlags : uint32_t
{
    UiReasonNone = 0u,
    UiReasonPauseOrMap = 1u << 0u,
    UiReasonLoading = 1u << 1u,
    UiReasonPhone = 1u << 2u,
};

#pragma pack(push, 8)
struct EyeResource
{
    uint64_t textureHandle;
};

struct ResourceSlot
{
    EyeResource eyes[EyeCount];
    uint64_t transactionId;
};

// publicationSequence is odd while the producer writes and even when stable.
// A transaction is acceptable only when both eye handles are nonzero and
// different, the slot transaction matches transactionId, and the ready fence
// has reached transactionId. The producer cannot reuse that slot until the
// release fence reaches the same value.
struct FrameBridge
{
    uint32_t magic;
    uint32_t version;
    uint32_t structBytes;
    uint32_t slotCount;
    volatile int32_t publicationSequence;
    uint32_t producerPid;
    uint64_t producerEpoch;
    uint64_t resourceSetId;
    uint64_t adapterLuid;
    uint32_t width;
    uint32_t height;
    PixelFormat format;
    uint32_t flags;
    uint32_t currentSlot;
    uint32_t resourceGeneration;
    uint64_t transactionId;
    uint64_t sourceFrameId[EyeCount];
    uint64_t poseSequence[EyeCount];
    int64_t renderedDisplayTime[EyeCount];
    uint64_t heartbeatTickMs;
    uint64_t readyFenceHandle;
    uint64_t releaseFenceHandle;
    ResourceSlot slots[SlotCount];
    uint64_t mappingBytes;
    PresentationMode presentationMode;
    uint32_t uiReasonFlags;
    uint32_t uiEye;
    uint32_t reserved32;
    uint64_t reserved64;
};
#pragma pack(pop)

static_assert(sizeof(EyeResource) == 8u, "EyeResource ABI changed");
static_assert(sizeof(ResourceSlot) == 24u, "ResourceSlot ABI changed");
static_assert(sizeof(FrameBridge) == 256u, "FrameBridge ABI changed");
static_assert(offsetof(FrameBridge, producerEpoch) == 24u, "FrameBridge layout changed");
static_assert(offsetof(FrameBridge, transactionId) == 72u, "FrameBridge layout changed");
static_assert(offsetof(FrameBridge, slots) == 152u, "FrameBridge layout changed");
static_assert(offsetof(FrameBridge, mappingBytes) == 224u, "FrameBridge layout changed");
static_assert(offsetof(FrameBridge, presentationMode) == 232u, "FrameBridge layout changed");
}
