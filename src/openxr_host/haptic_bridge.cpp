#include "haptic_bridge.h"

#include "../bridge/gtaiv_xr_haptic_bridge.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

namespace gtaiv_xr_host
{
namespace
{
using SharedHapticBridge = gtaiv_xr_bridge::HapticBridge;

bool stableSnapshot(
    const SharedHapticBridge* shared,
    SharedHapticBridge& output)
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

float amplitude(uint16_t motor)
{
    return static_cast<float>(motor) / 65535.0f;
}
}

struct HapticBridge::Implementation
{
    explicit Implementation(LogFunction function)
        : logger(std::move(function))
    {
    }

    ~Implementation()
    {
        reset();
    }

    void log(const std::string& message) const
    {
        if (logger)
            logger(message);
    }

    void closeProcess()
    {
        if (producerProcess)
        {
            CloseHandle(producerProcess);
            producerProcess = nullptr;
        }
        connectedPid = 0u;
        connectedEpoch = 0u;
    }

    void closeMapping()
    {
        closeProcess();
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
        lastMappingAttemptTick = GetTickCount64();
    }

    void reset()
    {
        closeMapping();
        unavailableLogged = false;
    }

    bool ensureMapping()
    {
        if (mapping && shared)
            return true;
        const uint64_t now = GetTickCount64();
        if (now - lastMappingAttemptTick < 1000u)
            return false;
        lastMappingAttemptTick = now;
        mapping = OpenFileMappingW(
            FILE_MAP_READ,
            FALSE,
            gtaiv_xr_bridge::HapticMappingName);
        if (!mapping)
            return false;
        shared = static_cast<const SharedHapticBridge*>(MapViewOfFile(
            mapping,
            FILE_MAP_READ,
            0u,
            0u,
            sizeof(SharedHapticBridge)));
        if (!shared)
        {
            CloseHandle(mapping);
            mapping = nullptr;
            return false;
        }
        return true;
    }

    bool processActive(uint32_t pid) const
    {
        DWORD exitCode = 0u;
        return producerProcess
            && GetProcessId(producerProcess) == pid
            && WaitForSingleObject(producerProcess, 0u) == WAIT_TIMEOUT
            && GetExitCodeProcess(producerProcess, &exitCode)
            && exitCode == STILL_ACTIVE;
    }

    bool connectProducer(const SharedHapticBridge& value)
    {
        closeProcess();
        producerProcess = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE,
            value.producerPid);
        if (!producerProcess || !processActive(value.producerPid))
        {
            closeProcess();
            return false;
        }
        connectedPid = value.producerPid;
        connectedEpoch = value.producerEpoch;
        log("XRHost: GTA XInput vibration bridge connected");
        return true;
    }

    HapticCommand update()
    {
        if (!ensureMapping())
            return {};

        SharedHapticBridge value {};
        if (!stableSnapshot(shared, value))
            return {};

        const uint64_t now = GetTickCount64();
        const bool valid =
            value.magic == gtaiv_xr_bridge::HapticMagic
            && value.version == gtaiv_xr_bridge::HapticVersion
            && value.structBytes == sizeof(SharedHapticBridge)
            && value.producerPid != 0u
            && value.producerEpoch != 0u
            && value.commandId != 0u
            && value.heartbeatTickMs != 0u
            && now >= value.heartbeatTickMs
            && now - value.heartbeatTickMs <= 250u;
        if (!valid)
        {
            if (connectedPid != 0u && !unavailableLogged)
            {
                log(
                    "XRHost: GTA vibration heartbeat unavailable; "
                    "Touch haptics stopped");
                unavailableLogged = true;
            }
            return {};
        }

        if (connectedPid != value.producerPid
            || connectedEpoch != value.producerEpoch)
        {
            if (!connectProducer(value))
                return {};
        }
        if (!processActive(value.producerPid))
        {
            log("XRHost: GTA vibration producer exited; Touch haptics stopped");
            closeMapping();
            return {};
        }

        unavailableLogged = false;
        HapticCommand command {};
        command.producerEpoch = value.producerEpoch;
        command.commandId = value.commandId;
        command.leftAmplitude = amplitude(value.leftMotor);
        command.rightAmplitude = amplitude(value.rightMotor);
        command.active =
            (value.flags & gtaiv_xr_bridge::HapticProducerRunning) != 0u;
        return command;
    }

    LogFunction logger;
    HANDLE mapping = nullptr;
    const SharedHapticBridge* shared = nullptr;
    HANDLE producerProcess = nullptr;
    uint32_t connectedPid = 0u;
    uint64_t connectedEpoch = 0u;
    uint64_t lastMappingAttemptTick = 0u;
    bool unavailableLogged = false;
};

HapticBridge::HapticBridge() = default;

HapticBridge::~HapticBridge()
{
    reset();
}

void HapticBridge::initialize(LogFunction logger)
{
    reset();
    implementation_ = new (std::nothrow) Implementation(std::move(logger));
}

HapticCommand HapticBridge::update()
{
    return implementation_ ? implementation_->update() : HapticCommand {};
}

void HapticBridge::reset()
{
    delete implementation_;
    implementation_ = nullptr;
}
}
