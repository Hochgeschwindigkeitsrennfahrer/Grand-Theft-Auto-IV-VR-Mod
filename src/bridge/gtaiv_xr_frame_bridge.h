#pragma once

#include <cstddef>
#include <cstdint>

// Pointer-free x86 <-> x64 contract for one complete stereo GPU transaction.
// Handles are values in the producer process and must be duplicated by the
// consumer from producerPid before they are opened.
namespace gtaiv_xr_bridge
{
constexpr wchar_t MappingName[] = L"Local\\GTAIV_XR_FrameBridge_v6";
constexpr uint32_t Magic = 0x46525847u;  // "GXRF" in little-endian memory.
constexpr uint32_t Version = 6u;
constexpr uint32_t EyeCount = 2u;
constexpr uint32_t SlotCount = 3u;
constexpr wchar_t CpuFrameMappingName[] =
    L"Local\\GTAIV_XR_CpuFrame_v2";
constexpr uint32_t CpuFrameMagic = 0x50435847u; // "GXCP"
constexpr uint32_t CpuFrameVersion = 2u;
constexpr uint32_t CpuFrameMaxWidth = 1280u;
constexpr uint32_t CpuFrameMaxHeight = 720u;
constexpr uint32_t CpuFrameBytesPerPixel = 4u;
constexpr uint32_t CpuFrameEyeCount = EyeCount;
constexpr uint64_t CpuFrameEyeBytes =
    static_cast<uint64_t>(CpuFrameMaxWidth)
    * static_cast<uint64_t>(CpuFrameMaxHeight)
    * CpuFrameBytesPerPixel;
// Every slot reserves both eyes. Mono/UI transactions publish one source eye
// and the host duplicates it into two private textures; validated world-stereo
// transactions publish both images atomically under the same slot sequence.
constexpr uint64_t CpuFrameSlotBytes =
    static_cast<uint64_t>(CpuFrameEyeCount) * CpuFrameEyeBytes;

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
    VerifiedWvpStereo = 1u << 7u,
    ImmersiveMono = 1u << 8u,
    // Proven FNVVR-style advancing CPU mailbox. This is the isolated
    // reliability path. It supports mono/UI and an atomic distinct-eye pair
    // without placing a host release fence on GTA/DXVK's graphics queue.
    CpuBgraMailbox = 1u << 9u,
    // A same-tick L/R pair created by the known working DrawScene x2 seam.
    // This is deliberately distinct from the stricter WVP-draw proof above.
    VerifiedDrawSceneStereo = 1u << 10u,
    // A same-tick L/R pair created at GTA IV CE's proven Mode 204 parent-dual
    // seam. This route must also carry FirstPersonCamera and NativeHeadHidden;
    // the x64 host then requires the exact shared capture pose before submit.
    VerifiedParentDualStereo = 1u << 11u,
    // The accepted world pair was rendered by the forced first-person camera
    // path. This is a proof bit, not a request for the host to move a camera.
    FirstPersonCamera = 1u << 12u,
    // GTA's native player-head suppression was operational for this pair.
    // It is required with VerifiedParentDualStereo so a third-person or
    // inside-head frame cannot be mislabeled as the Mode 58 shipping route.
    NativeHeadHidden = 1u << 13u,
};

enum class PresentationMode : uint32_t
{
    Unknown = 0u,
    WorldStereo = 1u,
    UiQuad = 2u,
    WorldMono = 3u,
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

// Pixels follow this header as SlotCount fixed-size BGRA slots. slotSequence
// is odd while that slot is being written and even while stable. The consumer
// accepts a slot only when the sequence is unchanged across its memcpy and the
// slot transaction matches FrameBridge::transactionId.
struct CpuFrameMailbox
{
    uint32_t magic;
    uint32_t version;
    uint32_t headerBytes;
    uint32_t slotCount;
    uint32_t maxWidth;
    uint32_t maxHeight;
    uint32_t bytesPerPixel;
    uint32_t eyeCount;
    volatile int32_t slotSequence[SlotCount];
    uint32_t reserved1;
    uint64_t slotTransactionId[SlotCount];
    uint64_t mappingBytes;
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
    uint32_t contentWidth;
    uint32_t contentHeight;
    uint32_t reserved32;
};
#pragma pack(pop)

static_assert(sizeof(EyeResource) == 8u, "EyeResource ABI changed");
static_assert(sizeof(ResourceSlot) == 24u, "ResourceSlot ABI changed");
static_assert(sizeof(CpuFrameMailbox) == 80u, "CPU mailbox ABI changed");
static_assert(sizeof(FrameBridge) == 256u, "FrameBridge ABI changed");
static_assert(offsetof(FrameBridge, producerEpoch) == 24u, "FrameBridge layout changed");
static_assert(offsetof(FrameBridge, transactionId) == 72u, "FrameBridge layout changed");
static_assert(offsetof(FrameBridge, slots) == 152u, "FrameBridge layout changed");
static_assert(offsetof(FrameBridge, mappingBytes) == 224u, "FrameBridge layout changed");
static_assert(offsetof(FrameBridge, presentationMode) == 232u, "FrameBridge layout changed");
constexpr uint64_t CpuFrameMappingBytes =
    sizeof(CpuFrameMailbox) + SlotCount * CpuFrameSlotBytes;
}
