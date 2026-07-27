#pragma once

#include <cstddef>
#include <cstdint>

// Fixed, pointer-free game-to-host contract for XInput vibration. The x86 ASI
// publishes the newest requested motor values; the x64 OpenXR host translates
// them into per-hand OpenXR haptics.
namespace gtaiv_xr_bridge
{
constexpr wchar_t HapticMappingName[] =
    L"Local\\GTAIV_XR_HapticBridge_v1";
constexpr uint32_t HapticMagic = 0x48525847u; // "GXRH"
constexpr uint32_t HapticVersion = 1u;

enum HapticBridgeFlags : uint32_t
{
    HapticProducerRunning = 1u << 0u,
};

#pragma pack(push, 8)
struct HapticBridge
{
    uint32_t magic;
    uint32_t version;
    uint32_t structBytes;
    uint32_t producerPid;
    volatile int32_t publicationSequence;
    uint32_t flags;
    uint64_t producerEpoch;
    uint64_t commandId;
    uint64_t heartbeatTickMs;
    uint16_t leftMotor;
    uint16_t rightMotor;
    uint32_t reserved0;
    uint64_t reserved1;
};
#pragma pack(pop)

static_assert(sizeof(HapticBridge) == 64u, "HapticBridge ABI changed");
static_assert(
    offsetof(HapticBridge, producerEpoch) == 24u,
    "HapticBridge layout changed");
static_assert(
    offsetof(HapticBridge, heartbeatTickMs) == 40u,
    "HapticBridge layout changed");
}
