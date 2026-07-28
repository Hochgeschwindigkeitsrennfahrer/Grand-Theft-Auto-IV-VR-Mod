#include "openxr_controller.h"

#include "log.h"
#include "openxr_controller_map.h"
#include "openxr_pose_client.h"
#include "../bridge/gtaiv_xr_haptic_bridge.h"
#include "../bridge/gtaiv_xr_pose_bridge.h"

#include "../../thirdparty/minhook/include/MinHook.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace asi
{
namespace
{
using gtaiv_xr_bridge::HapticBridge;
using gtaiv_xr_bridge::PoseBridge;
using controller_map::controllerInputAvailable;
using controller_map::motorHigh;
using controller_map::motorLow;
using controller_map::packMotors;
using controller_map::synthesizeGamepad;

using XInputGetStateFunction =
    DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputGetCapabilitiesFunction =
    DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using XInputSetStateFunction =
    DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using XInputEnableFunction = void(WINAPI*)(BOOL);

XInputGetStateFunction g_realGetState = nullptr;
XInputGetCapabilitiesFunction g_realGetCapabilities = nullptr;
XInputSetStateFunction g_realSetState = nullptr;
XInputEnableFunction g_realEnable = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_xinputEnabled{true};
std::atomic<uint32_t> g_requestedMotors{0u};

SRWLOCK g_packetLock = SRWLOCK_INIT;
XINPUT_GAMEPAD g_lastGamepad {};
DWORD g_packetNumber = 0u;

HANDLE g_hapticMapping = nullptr;
HapticBridge* g_hapticShared = nullptr;
uint64_t g_hapticEpoch = 0u;
uint64_t g_hapticCommandId = 0u;
uint32_t g_lastEffectiveMotors = 0u;

bool touchInputAvailable(PoseBridge& pose)
{
    if (!g_xinputEnabled.load(std::memory_order_acquire)
        || !GetLatestOpenXrPoseBridge(&pose))
    {
        return false;
    }
    const uint32_t required =
        gtaiv_xr_bridge::PoseBridgeHostRunning
        | gtaiv_xr_bridge::PoseBridgeSessionRunning
        | gtaiv_xr_bridge::PoseBridgeInputFocused;
    return (pose.flags & required) == required
        && (controllerInputAvailable(pose, 0u)
            || controllerInputAvailable(pose, 1u));
}

DWORD packetFor(const XINPUT_GAMEPAD& gamepad)
{
    AcquireSRWLockExclusive(&g_packetLock);
    if (std::memcmp(&g_lastGamepad, &gamepad, sizeof(gamepad)) != 0)
    {
        g_lastGamepad = gamepad;
        ++g_packetNumber;
        if (g_packetNumber == 0u)
            ++g_packetNumber;
    }
    const DWORD result = g_packetNumber;
    ReleaseSRWLockExclusive(&g_packetLock);
    return result;
}

DWORD WINAPI HookXInputGetState(
    DWORD userIndex,
    XINPUT_STATE* state)
{
    PoseBridge pose {};
    XINPUT_GAMEPAD gamepad {};
    if (userIndex == 0u
        && state
        && touchInputAvailable(pose)
        && synthesizeGamepad(pose, gamepad))
    {
        state->dwPacketNumber = packetFor(gamepad);
        state->Gamepad = gamepad;
        return ERROR_SUCCESS;
    }
    return g_realGetState
        ? g_realGetState(userIndex, state)
        : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI HookXInputGetCapabilities(
    DWORD userIndex,
    DWORD flags,
    XINPUT_CAPABILITIES* capabilities)
{
    PoseBridge pose {};
    if (userIndex == 0u
        && capabilities
        && touchInputAvailable(pose))
    {
        *capabilities = {};
        capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
        capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        capabilities->Flags = XINPUT_CAPS_FFB_SUPPORTED;
        capabilities->Gamepad.wButtons =
            XINPUT_GAMEPAD_DPAD_UP
            | XINPUT_GAMEPAD_DPAD_DOWN
            | XINPUT_GAMEPAD_DPAD_LEFT
            | XINPUT_GAMEPAD_DPAD_RIGHT
            | XINPUT_GAMEPAD_START
            | XINPUT_GAMEPAD_BACK
            | XINPUT_GAMEPAD_LEFT_THUMB
            | XINPUT_GAMEPAD_RIGHT_THUMB
            | XINPUT_GAMEPAD_LEFT_SHOULDER
            | XINPUT_GAMEPAD_RIGHT_SHOULDER
            | XINPUT_GAMEPAD_A
            | XINPUT_GAMEPAD_B
            | XINPUT_GAMEPAD_X
            | XINPUT_GAMEPAD_Y;
        capabilities->Gamepad.bLeftTrigger = 0xffu;
        capabilities->Gamepad.bRightTrigger = 0xffu;
        capabilities->Gamepad.sThumbLX = 32767;
        capabilities->Gamepad.sThumbLY = 32767;
        capabilities->Gamepad.sThumbRX = 32767;
        capabilities->Gamepad.sThumbRY = 32767;
        capabilities->Vibration.wLeftMotorSpeed = 0xffffu;
        capabilities->Vibration.wRightMotorSpeed = 0xffffu;
        return ERROR_SUCCESS;
    }
    return g_realGetCapabilities
        ? g_realGetCapabilities(userIndex, flags, capabilities)
        : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI HookXInputSetState(
    DWORD userIndex,
    XINPUT_VIBRATION* vibration)
{
    PoseBridge pose {};
    const bool touchActive =
        userIndex == 0u
        && vibration
        && touchInputAvailable(pose);
    if (touchActive)
    {
        g_requestedMotors.store(
            packMotors(
                vibration->wLeftMotorSpeed,
                vibration->wRightMotorSpeed),
            std::memory_order_release);
    }

    const DWORD physicalResult = g_realSetState
        ? g_realSetState(userIndex, vibration)
        : ERROR_DEVICE_NOT_CONNECTED;
    return touchActive ? ERROR_SUCCESS : physicalResult;
}

void WINAPI HookXInputEnable(BOOL enable)
{
    g_xinputEnabled.store(enable != FALSE, std::memory_order_release);
    if (enable == FALSE)
        g_requestedMotors.store(0u, std::memory_order_release);
    if (g_realEnable)
        g_realEnable(enable);
}

bool installHook(
    void* target,
    void* detour,
    void** original,
    const char* name)
{
    if (!target
        || MH_CreateHook(target, detour, original) != MH_OK
        || MH_EnableHook(target) != MH_OK)
    {
        Log("OpenXRController: XInput hook failed for %s", name);
        return false;
    }
    return true;
}

void commitHaptic(const HapticBridge& value)
{
    if (!g_hapticShared)
        return;
    volatile LONG* sequence =
        reinterpret_cast<volatile LONG*>(
            &g_hapticShared->publicationSequence);
    LONG current = InterlockedCompareExchange(sequence, 0, 0);
    LONG writing = (current & 1) ? current + 2 : current + 1;
    InterlockedExchange(sequence, writing);
    MemoryBarrier();

    constexpr size_t prefixBytes =
        offsetof(HapticBridge, publicationSequence);
    constexpr size_t suffixOffset =
        offsetof(HapticBridge, publicationSequence)
        + sizeof(int32_t);
    std::memcpy(g_hapticShared, &value, prefixBytes);
    std::memcpy(
        reinterpret_cast<uint8_t*>(g_hapticShared) + suffixOffset,
        reinterpret_cast<const uint8_t*>(&value) + suffixOffset,
        sizeof(HapticBridge) - suffixOffset);
    MemoryBarrier();
    InterlockedExchange(sequence, writing + 1);
}

void publishHaptic(bool running, uint32_t effectiveMotors)
{
    if (!g_hapticShared)
        return;
    if (effectiveMotors != g_lastEffectiveMotors)
    {
        g_lastEffectiveMotors = effectiveMotors;
        ++g_hapticCommandId;
        if (g_hapticCommandId == 0u)
            ++g_hapticCommandId;
    }

    HapticBridge value {};
    value.magic = gtaiv_xr_bridge::HapticMagic;
    value.version = gtaiv_xr_bridge::HapticVersion;
    value.structBytes = sizeof(HapticBridge);
    value.producerPid = GetCurrentProcessId();
    value.flags =
        running ? gtaiv_xr_bridge::HapticProducerRunning : 0u;
    value.producerEpoch = g_hapticEpoch;
    value.commandId = g_hapticCommandId;
    value.heartbeatTickMs = GetTickCount64();
    value.leftMotor = motorLow(effectiveMotors);
    value.rightMotor = motorHigh(effectiveMotors);
    commitHaptic(value);
}

bool createHapticMapping()
{
    g_hapticMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0u,
        sizeof(HapticBridge),
        gtaiv_xr_bridge::HapticMappingName);
    if (!g_hapticMapping)
        return false;
    g_hapticShared = static_cast<HapticBridge*>(MapViewOfFile(
        g_hapticMapping,
        FILE_MAP_READ | FILE_MAP_WRITE,
        0u,
        0u,
        sizeof(HapticBridge)));
    if (!g_hapticShared)
    {
        CloseHandle(g_hapticMapping);
        g_hapticMapping = nullptr;
        return false;
    }

    LARGE_INTEGER counter {};
    QueryPerformanceCounter(&counter);
    g_hapticEpoch =
        static_cast<uint64_t>(counter.QuadPart)
        ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32u)
        ^ GetTickCount64();
    if (g_hapticEpoch == 0u)
        g_hapticEpoch = 1u;
    g_hapticCommandId = 1u;
    publishHaptic(false, 0u);
    return true;
}
}

bool InstallOpenXrControllerHooks()
{
    if (g_installed.load(std::memory_order_acquire))
        return true;

    const MH_STATUS initializeResult = MH_Initialize();
    if (initializeResult != MH_OK
        && initializeResult != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("OpenXRController: MinHook initialization failed");
        return false;
    }

    HMODULE xinput = GetModuleHandleW(L"xinput9_1_0.dll");
    if (!xinput)
        xinput = LoadLibraryW(L"xinput9_1_0.dll");
    if (!xinput)
    {
        Log("OpenXRController: system xinput9_1_0.dll unavailable");
        return false;
    }

    void* getState =
        reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState"));
    void* getCapabilities =
        reinterpret_cast<void*>(GetProcAddress(
            xinput,
            "XInputGetCapabilities"));
    void* setState =
        reinterpret_cast<void*>(GetProcAddress(xinput, "XInputSetState"));
    void* enable =
        reinterpret_cast<void*>(GetProcAddress(xinput, "XInputEnable"));

    if (!installHook(
            getState,
            reinterpret_cast<void*>(&HookXInputGetState),
            reinterpret_cast<void**>(&g_realGetState),
            "XInputGetState"))
    {
        return false;
    }
    if (!installHook(
            getCapabilities,
            reinterpret_cast<void*>(&HookXInputGetCapabilities),
            reinterpret_cast<void**>(&g_realGetCapabilities),
            "XInputGetCapabilities"))
    {
        MH_DisableHook(getState);
        MH_RemoveHook(getState);
        g_realGetState = nullptr;
        return false;
    }
    if (!installHook(
            setState,
            reinterpret_cast<void*>(&HookXInputSetState),
            reinterpret_cast<void**>(&g_realSetState),
            "XInputSetState"))
    {
        MH_DisableHook(getCapabilities);
        MH_RemoveHook(getCapabilities);
        MH_DisableHook(getState);
        MH_RemoveHook(getState);
        g_realGetCapabilities = nullptr;
        g_realGetState = nullptr;
        return false;
    }
    if (enable)
    {
        if (!installHook(
                enable,
                reinterpret_cast<void*>(&HookXInputEnable),
                reinterpret_cast<void**>(&g_realEnable),
                "XInputEnable"))
        {
            Log(
                "OpenXRController: XInputEnable hook unavailable; "
                "state/capabilities/vibration remain active");
        }
    }

    if (!createHapticMapping())
    {
        Log(
            "OpenXRController: vibration bridge unavailable; "
            "Touch input remains active without haptics");
    }
    g_installed.store(true, std::memory_order_release);
    Log(
        "OpenXRController: Touch -> XInput ready "
        "(sticks/triggers/A-B-X-Y/shoulders/thumbs/Start/Back/D-pad)");
    return true;
}

void PumpOpenXrControllerBridge()
{
    if (!g_installed.load(std::memory_order_acquire)
        || !g_hapticShared)
    {
        return;
    }
    PoseBridge pose {};
    const bool running = touchInputAvailable(pose);
    if (!running)
        g_requestedMotors.store(0u, std::memory_order_release);
    const uint32_t motors = running
        ? g_requestedMotors.load(std::memory_order_acquire)
        : 0u;
    publishHaptic(running, motors);
}

void ShutdownOpenXrControllerBridge()
{
    if (g_hapticShared)
    {
        publishHaptic(false, 0u);
        UnmapViewOfFile(g_hapticShared);
        g_hapticShared = nullptr;
    }
    if (g_hapticMapping)
    {
        CloseHandle(g_hapticMapping);
        g_hapticMapping = nullptr;
    }
}
}
