#include "openxr_pose_client.h"

#include "log.h"
#include "../bridge/gtaiv_xr_pose_bridge.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace asi
{
namespace
{
using gtaiv_xr_bridge::PoseBridge;

class PoseBridgeClient
{
public:
    ~PoseBridgeClient()
    {
        reset();
    }

    void poll()
    {
        if (!ensureMapping())
            return;

        PoseBridge snapshot {};
        if (!readStableSnapshot(snapshot))
            return;

        const uint64_t now = GetTickCount64();
        if (!validHeader(snapshot)
            || (snapshot.flags & gtaiv_xr_bridge::PoseBridgeHostRunning) == 0u
            || now - snapshot.heartbeatTickMs > kMaxHeartbeatAgeMs)
        {
            logUnavailableOnce("host stopped, packet invalid, or heartbeat stale");
            haveValidSample_ = false;
            closeMapping();
            return;
        }

        unavailableLogged_ = false;
        if (!haveValidSample_ || hostEpoch_ != snapshot.producerEpoch)
        {
            hostEpoch_ = snapshot.producerEpoch;
            haveValidSample_ = true;
            Log(
                "OpenXRPose: host connected epoch=%llu pid=%lu abi=%u bytes=%u",
                static_cast<unsigned long long>(snapshot.producerEpoch),
                static_cast<unsigned long>(snapshot.producerPid),
                snapshot.version,
                snapshot.structBytes);
        }

        ++sampleCount_;
        const bool controllerChanged = controllerChangedEnough(snapshot);
        if (sampleCount_ == 1u || sampleCount_ % 240u == 0u || controllerChanged)
        {
            const float ageMs = static_cast<float>(now - snapshot.heartbeatTickMs);
            Log(
                "OpenXRPose: seq=%d frame=%llu ageMs=%.0f flags=0x%08lx "
                "hmd=(%.3f,%.3f,%.3f) L[t=%.2f s=%.2f stick=%.2f,%.2f btn=0x%03lx act=0x%04lx] "
                "R[t=%.2f s=%.2f stick=%.2f,%.2f btn=0x%03lx act=0x%04lx]",
                snapshot.publicationSequence,
                static_cast<unsigned long long>(snapshot.frameId),
                ageMs,
                static_cast<unsigned long>(snapshot.flags),
                snapshot.hmdPose.position[0],
                snapshot.hmdPose.position[1],
                snapshot.hmdPose.position[2],
                snapshot.controllers[0].trigger,
                snapshot.controllers[0].squeeze,
                snapshot.controllers[0].thumbstickX,
                snapshot.controllers[0].thumbstickY,
                static_cast<unsigned long>(snapshot.controllers[0].buttons),
                static_cast<unsigned long>(snapshot.controllers[0].activeFlags),
                snapshot.controllers[1].trigger,
                snapshot.controllers[1].squeeze,
                snapshot.controllers[1].thumbstickX,
                snapshot.controllers[1].thumbstickY,
                static_cast<unsigned long>(snapshot.controllers[1].buttons),
                static_cast<unsigned long>(snapshot.controllers[1].activeFlags));
        }
    }

    void reset()
    {
        closeMapping();
        sampleCount_ = 0u;
        hostEpoch_ = 0u;
        haveValidSample_ = false;
        unavailableLogged_ = false;
        haveControllerSample_ = false;
    }

private:
    static constexpr uint64_t kMappingRetryMs = 1000u;
    static constexpr uint64_t kMaxHeartbeatAgeMs = 250u;

    void closeMapping()
    {
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
        lastMappingAttemptTick_ = GetTickCount64();
    }

    bool ensureMapping()
    {
        if (mapping_ && shared_)
            return true;

        const uint64_t now = GetTickCount64();
        if (now - lastMappingAttemptTick_ < kMappingRetryMs)
            return false;
        lastMappingAttemptTick_ = now;

        mapping_ = OpenFileMappingW(
            FILE_MAP_READ,
            FALSE,
            gtaiv_xr_bridge::PoseMappingName);
        if (!mapping_)
            return false;

        shared_ = static_cast<const PoseBridge*>(MapViewOfFile(
            mapping_,
            FILE_MAP_READ,
            0u,
            0u,
            sizeof(PoseBridge)));
        if (!shared_)
        {
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }
        Log("OpenXRPose: x64 host mapping found; awaiting a stable sample");
        return true;
    }

    bool readStableSnapshot(PoseBridge& output) const
    {
        if (!shared_)
            return false;

        const int32_t first = shared_->publicationSequence;
        if (first == 0 || (first & 1) != 0)
            return false;
        MemoryBarrier();
        std::memcpy(&output, shared_, sizeof(output));
        MemoryBarrier();
        const int32_t second = shared_->publicationSequence;
        return first == second && (second & 1) == 0;
    }

    static bool validHeader(const PoseBridge& value)
    {
        return value.magic == gtaiv_xr_bridge::PoseMagic
            && value.version == gtaiv_xr_bridge::PoseVersion
            && value.structBytes == sizeof(PoseBridge)
            && value.producerPid != 0u
            && value.producerEpoch != 0u;
    }

    bool controllerChangedEnough(const PoseBridge& value)
    {
        const auto changed = [](float before, float after) {
            return std::fabs(before - after) >= 0.10f;
        };
        if (!haveControllerSample_)
        {
            lastControllerSample_ = value;
            haveControllerSample_ = true;
            return true;
        }

        for (uint32_t hand = 0u; hand < 2u; ++hand)
        {
            const auto& before = lastControllerSample_.controllers[hand];
            const auto& after = value.controllers[hand];
            if (before.buttons != after.buttons
                || before.activeFlags != after.activeFlags
                || changed(before.trigger, after.trigger)
                || changed(before.squeeze, after.squeeze)
                || changed(before.thumbstickX, after.thumbstickX)
                || changed(before.thumbstickY, after.thumbstickY))
            {
                lastControllerSample_ = value;
                return true;
            }
        }
        return false;
    }

    void logUnavailableOnce(const char* reason)
    {
        if (!unavailableLogged_)
        {
            Log("OpenXRPose: unavailable (%s); GTA remains flat and neutral", reason);
            unavailableLogged_ = true;
        }
    }

    HANDLE mapping_ = nullptr;
    const PoseBridge* shared_ = nullptr;
    uint64_t lastMappingAttemptTick_ = 0u;
    uint64_t sampleCount_ = 0u;
    uint64_t hostEpoch_ = 0u;
    PoseBridge lastControllerSample_ {};
    bool haveValidSample_ = false;
    bool unavailableLogged_ = false;
    bool haveControllerSample_ = false;
};

PoseBridgeClient g_client;
}

void PollOpenXrPoseBridge()
{
    g_client.poll();
}

void ShutdownOpenXrPoseBridge()
{
    g_client.reset();
}
}
