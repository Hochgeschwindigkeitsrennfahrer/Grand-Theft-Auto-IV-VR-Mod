#pragma once

#include <cstddef>
#include <cstdint>

namespace gtaiv_xr_bridge
{
constexpr wchar_t MappingName[] = L"Local\\GTAIV_XR_FrameBridge_v1";
constexpr uint32_t Magic = 0x46525847u;  // "GXRF" in little-endian memory.
constexpr uint32_t Version = 1u;
constexpr uint32_t SlotCount = 3u;

enum class PixelFormat : uint32_t
{
    Unknown = 0u,
    B8G8R8A8Unorm = 1u,
    B8G8R8X8Unorm = 2u,
    R8G8B8A8Unorm = 3u,
};

enum Flags : uint32_t
{
    Running = 1u << 0u,
    Mono = 1u << 1u,
    CpuPixels = 1u << 2u,
};

#pragma pack(push, 8)
struct FrameSlot
{
    uint64_t textureHandle;
    uint64_t readyValue;
};

// This is a pointer-free, fixed-width ABI shared by the x86 ASI and x64 host.
// publicationSequence is odd while the producer writes and even when stable.
struct FrameBridge
{
    uint32_t magic;
    uint32_t version;
    uint32_t structBytes;
    uint32_t slotCount;
    volatile int32_t publicationSequence;
    uint32_t producerPid;
    uint64_t producerEpoch;
    uint64_t adapterLuid;
    uint32_t width;
    uint32_t height;
    PixelFormat format;
    uint32_t currentSlot;
    uint64_t frameId;
    uint64_t heartbeatTickMs;
    uint64_t readyFenceHandle;
    uint64_t releaseFenceHandle;
    FrameSlot slots[SlotCount];
    uint32_t flags;
    uint32_t resourceGeneration;
    uint32_t rowPitch;
    uint32_t reserved;
    uint64_t mappingBytes;
};

struct CpuSlotHeader
{
    volatile int32_t publicationSequence;
    uint32_t reserved;
    uint64_t frameId;
};
#pragma pack(pop)

static_assert(sizeof(FrameSlot) == 16u, "FrameSlot ABI changed");
static_assert(sizeof(FrameBridge) == 160u, "FrameBridge ABI changed");
static_assert(sizeof(CpuSlotHeader) == 16u, "CpuSlotHeader ABI changed");
static_assert(offsetof(FrameBridge, producerEpoch) == 24u, "FrameBridge layout changed");
static_assert(offsetof(FrameBridge, slots) == 88u, "FrameBridge layout changed");
}
