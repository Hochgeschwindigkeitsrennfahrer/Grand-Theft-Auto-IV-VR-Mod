#pragma once

#include <cstdint>
#include <string>

namespace gtaiv_xr_host
{
struct GamePresentationKey
{
    uint64_t transactionId = 0u;
    uint64_t sourceFrameId[2] = {};
    uint64_t poseSequence[2] = {};
    int64_t renderedDisplayTime[2] = {};
    uint32_t referenceSpaceGeneration = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t contentWidth = 0u;
    uint32_t contentHeight = 0u;
    uint32_t presentationMode = 0u;
    uint32_t uiReasonFlags = 0u;
    uint32_t uiEye = 0u;
    bool sameSimulationTick = false;
    bool temporalStereo = false;

    explicit operator bool() const noexcept
    {
        return transactionId != 0u;
    }
};

inline bool operator==(
    const GamePresentationKey& left,
    const GamePresentationKey& right) noexcept
{
    return left.transactionId == right.transactionId
        && left.sourceFrameId[0] == right.sourceFrameId[0]
        && left.sourceFrameId[1] == right.sourceFrameId[1]
        && left.poseSequence[0] == right.poseSequence[0]
        && left.poseSequence[1] == right.poseSequence[1]
        && left.renderedDisplayTime[0] == right.renderedDisplayTime[0]
        && left.renderedDisplayTime[1] == right.renderedDisplayTime[1]
        && left.referenceSpaceGeneration == right.referenceSpaceGeneration
        && left.width == right.width
        && left.height == right.height
        && left.contentWidth == right.contentWidth
        && left.contentHeight == right.contentHeight
        && left.presentationMode == right.presentationMode
        && left.uiReasonFlags == right.uiReasonFlags
        && left.uiEye == right.uiEye
        && left.sameSimulationTick == right.sameSimulationTick
        && left.temporalStereo == right.temporalStereo;
}

template<typename Payload>
class GamePresentationCache
{
public:
    bool tryReuse(
        const GamePresentationKey& key,
        Payload& payload) noexcept
    {
        if (!valid_ || !key || !(key_ == key))
            return false;
        payload = payload_;
        ++reuseCount_;
        return true;
    }

    void commit(
        const GamePresentationKey& key,
        const Payload& payload) noexcept
    {
        if (!key)
            return;
        key_ = key;
        payload_ = payload;
        valid_ = true;
        ++updateCount_;
    }

    void invalidate() noexcept
    {
        valid_ = false;
    }

    uint64_t updateCount() const noexcept
    {
        return updateCount_;
    }

    uint64_t reuseCount() const noexcept
    {
        return reuseCount_;
    }

private:
    GamePresentationKey key_ {};
    Payload payload_ {};
    uint64_t updateCount_ = 0u;
    uint64_t reuseCount_ = 0u;
    bool valid_ = false;
};

inline bool GamePresentationCacheSelfTest(std::string& failure)
{
    struct TestPayload
    {
        uint64_t leftPose = 0u;
        uint64_t rightPose = 0u;
        bool quad = false;
    };

    GamePresentationCache<TestPayload> cache;
    GamePresentationKey key {};
    key.transactionId = 42u;
    key.sourceFrameId[0] = 100u;
    key.sourceFrameId[1] = 101u;
    key.poseSequence[0] = 200u;
    key.poseSequence[1] = 201u;
    key.renderedDisplayTime[0] = 300;
    key.renderedDisplayTime[1] = 301;
    key.referenceSpaceGeneration = 1u;
    key.width = 1280u;
    key.height = 720u;
    key.contentWidth = 1280u;
    key.contentHeight = 720u;
    key.presentationMode = 2u;
    key.temporalStereo = true;

    TestPayload reused {};
    if (cache.tryReuse(key, reused))
    {
        failure = "an unprepared transaction was reused";
        return false;
    }

    const TestPayload prepared { 200u, 201u, false };
    cache.commit(key, prepared);
    for (uint32_t frame = 0u; frame < 600u; ++frame)
    {
        reused = {};
        if (!cache.tryReuse(key, reused)
            || reused.leftPose != prepared.leftPose
            || reused.rightPose != prepared.rightPose
            || reused.quad != prepared.quad)
        {
            failure = "a held transaction or its exact poses changed";
            return false;
        }
    }
    if (cache.updateCount() != 1u || cache.reuseCount() != 600u)
    {
        failure = "held-frame update/reuse counts are wrong";
        return false;
    }

    GamePresentationKey next = key;
    ++next.transactionId;
    ++next.sourceFrameId[0];
    ++next.sourceFrameId[1];
    if (cache.tryReuse(next, reused))
    {
        failure = "a new transaction reused the prior swapchain image";
        return false;
    }
    cache.commit(next, TestPayload { 202u, 203u, false });
    if (cache.updateCount() != 2u)
    {
        failure = "a new transaction did not require one new update";
        return false;
    }

    GamePresentationKey rerouted = next;
    ++rerouted.presentationMode;
    if (cache.tryReuse(rerouted, reused))
    {
        failure = "a route change reused the prior presentation";
        return false;
    }

    cache.invalidate();
    if (cache.tryReuse(next, reused))
    {
        failure = "an invalidated presentation was reused";
        return false;
    }
    return true;
}
}
