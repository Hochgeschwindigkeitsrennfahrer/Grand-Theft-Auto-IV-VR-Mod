#pragma once

#include <cstddef>
#include <cstdint>

// This header is intentionally pointer-free and compiled by both the x86 ASI
// and the x64 OpenXR host. It is the host-to-game half of the OpenXR contract.
namespace gtaiv_xr_bridge
{
constexpr wchar_t PoseMappingName[] = L"Local\\GTAIV_XR_PoseBridge_v1";
constexpr wchar_t PoseProducerMutexName[] =
    L"Local\\GTAIV_XR_PoseBridgeProducer_v1";
constexpr uint32_t PoseMagic = 0x50525847u; // "GXRP" in little-endian memory.
constexpr uint32_t PoseVersion = 1u;

enum PoseBridgeFlags : uint32_t
{
    PoseBridgeHostRunning = 1u << 0u,
    PoseBridgeSessionRunning = 1u << 1u,
    PoseBridgeViewsValid = 1u << 2u,
    PoseBridgeHmdValid = 1u << 3u,
    PoseBridgeLeftControllerValid = 1u << 4u,
    PoseBridgeRightControllerValid = 1u << 5u,
};

enum ControllerActiveFlags : uint32_t
{
    ControllerGripPose = 1u << 0u,
    ControllerAimPose = 1u << 1u,
    ControllerTrigger = 1u << 2u,
    ControllerSqueeze = 1u << 3u,
    ControllerThumbstick = 1u << 4u,
    ControllerPrimary = 1u << 5u,
    ControllerSecondary = 1u << 6u,
    ControllerMenu = 1u << 7u,
    ControllerThumbstickClick = 1u << 8u,
    ControllerTriggerClick = 1u << 9u,
    ControllerSqueezeClick = 1u << 10u,
    ControllerPrimaryTouch = 1u << 11u,
    ControllerSecondaryTouch = 1u << 12u,
    ControllerThumbstickTouch = 1u << 13u,
    ControllerThumbrestTouch = 1u << 14u,
};

enum ControllerButtons : uint32_t
{
    ControllerPrimaryClick = 1u << 0u,
    ControllerSecondaryClick = 1u << 1u,
    ControllerMenuClick = 1u << 2u,
    ControllerThumbstickPressed = 1u << 3u,
    ControllerTriggerPressed = 1u << 4u,
    ControllerSqueezePressed = 1u << 5u,
    ControllerPrimaryTouched = 1u << 6u,
    ControllerSecondaryTouched = 1u << 7u,
    ControllerThumbstickTouched = 1u << 8u,
    ControllerThumbrestTouched = 1u << 9u,
};

#pragma pack(push, 8)
struct Pose
{
    float orientation[4]; // x, y, z, w
    float position[3];    // meters, OpenXR LOCAL space
    float reserved;
};

struct EyeFov
{
    float angleLeft;
    float angleRight;
    float angleUp;
    float angleDown;
};

struct ControllerState
{
    Pose gripPose;
    Pose aimPose;
    float trigger;
    float squeeze;
    float thumbstickX;
    float thumbstickY;
    uint32_t buttons;
    uint32_t activeFlags;
    uint32_t reserved[2];
};

// publicationSequence is odd while the x64 host writes and even when a
// reader may accept the payload. No field depends on pointer width.
struct PoseBridge
{
    uint32_t magic;
    uint32_t version;
    uint32_t structBytes;
    uint32_t producerPid;
    volatile int32_t publicationSequence;
    uint32_t flags;
    uint64_t producerEpoch;
    uint64_t frameId;
    int64_t predictedDisplayTime;
    uint64_t heartbeatTickMs;
    uint32_t referenceSpaceGeneration;
    uint32_t recenterRequestId;
    Pose hmdPose;
    Pose eyePoses[2];
    EyeFov eyeFovs[2];
    ControllerState controllers[2];
};
#pragma pack(pop)

static_assert(sizeof(Pose) == 32u, "Pose ABI changed");
static_assert(sizeof(EyeFov) == 16u, "EyeFov ABI changed");
static_assert(sizeof(ControllerState) == 96u, "ControllerState ABI changed");
static_assert(sizeof(PoseBridge) == 384u, "PoseBridge ABI changed");
static_assert(offsetof(PoseBridge, producerEpoch) == 24u, "PoseBridge layout changed");
static_assert(offsetof(PoseBridge, hmdPose) == 64u, "PoseBridge layout changed");
static_assert(offsetof(PoseBridge, eyePoses) == 96u, "PoseBridge layout changed");
static_assert(offsetof(PoseBridge, eyeFovs) == 160u, "PoseBridge layout changed");
static_assert(offsetof(PoseBridge, controllers) == 192u, "PoseBridge layout changed");
}
