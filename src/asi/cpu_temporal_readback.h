#pragma once

#include <cstdint>

namespace asi
{
namespace cpu_temporal_readback
{
struct PairKey
{
    uint64_t pairId = 0u;
    uint64_t sourceFrameId[2] {};
    uint64_t poseSequence[2] {};
    int64_t renderedDisplayTime[2] {};
    uint64_t poseProducerEpoch = 0u;
    uint64_t resourceGeneration = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t contentWidth = 0u;
    uint32_t contentHeight = 0u;
    uint32_t format = 0u;
    uint32_t flags = 0u;
    uint32_t d3dThreadId = 0u;
    uint32_t referenceSpaceGeneration = 0u;

    bool valid() const
    {
        return pairId != 0u
            && sourceFrameId[0] != 0u
            && sourceFrameId[1] != 0u
            && poseSequence[0] != 0u
            && poseSequence[1] != 0u
            && renderedDisplayTime[0] != 0
            && renderedDisplayTime[1] != 0
            && poseProducerEpoch != 0u
            && resourceGeneration != 0u
            && width != 0u
            && height != 0u
            && format != 0u
            && d3dThreadId != 0u;
    }
};

inline bool sameKey(const PairKey& left, const PairKey& right)
{
    return left.pairId == right.pairId
        && left.sourceFrameId[0] == right.sourceFrameId[0]
        && left.sourceFrameId[1] == right.sourceFrameId[1]
        && left.poseSequence[0] == right.poseSequence[0]
        && left.poseSequence[1] == right.poseSequence[1]
        && left.renderedDisplayTime[0] == right.renderedDisplayTime[0]
        && left.renderedDisplayTime[1] == right.renderedDisplayTime[1]
        && left.poseProducerEpoch == right.poseProducerEpoch
        && left.resourceGeneration == right.resourceGeneration
        && left.width == right.width
        && left.height == right.height
        && left.contentWidth == right.contentWidth
        && left.contentHeight == right.contentHeight
        && left.format == right.format
        && left.flags == right.flags
        && left.d3dThreadId == right.d3dThreadId
        && left.referenceSpaceGeneration
            == right.referenceSpaceGeneration;
}

enum class ScheduleDecision
{
    Rejected,
    StageLeft,
    ReplaceWithLeft,
    WaitForFreshPose,
    FinishRight,
};

class Scheduler
{
public:
    ScheduleDecision decide(
        const PairKey& key,
        uint64_t poseFrameId,
        uint64_t producerCallOrdinal,
        uint64_t monotonicTimeMs,
        uint64_t maxPendingAgeMs)
    {
        if (!key.valid()
            || poseFrameId == 0u
            || producerCallOrdinal == 0u
            || monotonicTimeMs == 0u
            || maxPendingAgeMs == 0u)
        {
            reset();
            return ScheduleDecision::Rejected;
        }

        if (!active_)
        {
            stage(
                key,
                poseFrameId,
                producerCallOrdinal,
                monotonicTimeMs);
            return ScheduleDecision::StageLeft;
        }

        if (poseFrameId < gatePoseFrameId_
            || producerCallOrdinal <= gateCallOrdinal_
            || monotonicTimeMs < gateTimeMs_
            || monotonicTimeMs - gateTimeMs_ > maxPendingAgeMs)
        {
            reset();
            return ScheduleDecision::Rejected;
        }

        if (key.pairId != key_.pairId)
        {
            stage(
                key,
                poseFrameId,
                producerCallOrdinal,
                monotonicTimeMs);
            return ScheduleDecision::ReplaceWithLeft;
        }

        if (!sameKey(key, key_))
        {
            reset();
            return ScheduleDecision::Rejected;
        }

        if (poseFrameId == gatePoseFrameId_)
            return ScheduleDecision::WaitForFreshPose;

        return ScheduleDecision::FinishRight;
    }

    void complete()
    {
        reset();
    }

    void cancel()
    {
        reset();
    }

    bool active() const
    {
        return active_;
    }

    uint64_t pendingPairId() const
    {
        return active_ ? key_.pairId : 0u;
    }

    uint64_t gatePoseFrameId() const
    {
        return active_ ? gatePoseFrameId_ : 0u;
    }

    uint64_t gateCallOrdinal() const
    {
        return active_ ? gateCallOrdinal_ : 0u;
    }

    uint64_t gateTimeMs() const
    {
        return active_ ? gateTimeMs_ : 0u;
    }

private:
    void stage(
        const PairKey& key,
        uint64_t poseFrameId,
        uint64_t producerCallOrdinal,
        uint64_t monotonicTimeMs)
    {
        key_ = key;
        gatePoseFrameId_ = poseFrameId;
        gateCallOrdinal_ = producerCallOrdinal;
        gateTimeMs_ = monotonicTimeMs;
        active_ = true;
    }

    void reset()
    {
        key_ = {};
        gatePoseFrameId_ = 0u;
        gateCallOrdinal_ = 0u;
        gateTimeMs_ = 0u;
        active_ = false;
    }

    PairKey key_ {};
    uint64_t gatePoseFrameId_ = 0u;
    uint64_t gateCallOrdinal_ = 0u;
    uint64_t gateTimeMs_ = 0u;
    bool active_ = false;
};
}
}
