#include "../src/asi/cpu_temporal_readback.h"

#include <iostream>

namespace
{
using asi::cpu_temporal_readback::PairKey;
using asi::cpu_temporal_readback::ScheduleDecision;
using asi::cpu_temporal_readback::Scheduler;

int failures = 0;
constexpr uint64_t MaxPendingAgeMs = 250u;

ScheduleDecision decide(
    Scheduler& scheduler,
    const PairKey& pair,
    uint64_t poseFrameId,
    uint64_t producerCallOrdinal,
    uint64_t monotonicTimeMs)
{
    return scheduler.decide(
        pair,
        poseFrameId,
        producerCallOrdinal,
        monotonicTimeMs,
        MaxPendingAgeMs);
}

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

PairKey key(uint64_t pairId)
{
    PairKey value {};
    value.pairId = pairId;
    value.sourceFrameId[0] = pairId * 2u;
    value.sourceFrameId[1] = pairId * 2u + 1u;
    value.poseSequence[0] = pairId * 3u;
    value.poseSequence[1] = pairId * 3u + 1u;
    value.renderedDisplayTime[0] =
        static_cast<int64_t>(pairId * 100u);
    value.renderedDisplayTime[1] =
        static_cast<int64_t>(pairId * 100u + 1u);
    value.poseProducerEpoch = 7u;
    value.resourceGeneration = 1u;
    value.width = 1280u;
    value.height = 720u;
    value.contentWidth = 1280u;
    value.contentHeight = 720u;
    value.format = 21u;
    value.flags = 4u;
    value.d3dThreadId = 19u;
    value.referenceSpaceGeneration = 3u;
    return value;
}

void testFreshPoseGateAndAtomicCompletion()
{
    Scheduler scheduler;
    const PairKey pair = key(10u);
    check(
        decide(scheduler, pair, 100u, 1u, 1000u)
            == ScheduleDecision::StageLeft,
        "new pair did not stage its left eye");
    check(
        scheduler.active() && scheduler.pendingPairId() == 10u,
        "left stage did not retain the pair identity");
    check(
        decide(scheduler, pair, 100u, 500u, 1010u)
            == ScheduleDecision::WaitForFreshPose,
        "same host pose was allowed to finish despite many producer calls");
    check(
        decide(scheduler, pair, 101u, 900u, 1017u)
            == ScheduleDecision::FinishRight,
        "fresh host pose did not release the right-eye phase");
    check(
        scheduler.active(),
        "finish decision exposed completion before the caller committed");
    scheduler.complete();
    check(!scheduler.active(), "atomic completion did not clear pending state");
}

void testNewPairReplacesUnpublishedStage()
{
    Scheduler scheduler;
    check(
        decide(scheduler, key(20u), 200u, 1u, 2000u)
            == ScheduleDecision::StageLeft,
        "first pair did not stage");
    check(
        decide(scheduler, key(21u), 201u, 2u, 2001u)
            == ScheduleDecision::ReplaceWithLeft,
        "new pair did not replace the unpublished stage");
    check(
        scheduler.pendingPairId() == 21u
            && scheduler.gatePoseFrameId() == 201u,
        "replacement retained stale pair or pose identity");
}

void testSamePairMutationAndPoseRegressionReject()
{
    Scheduler scheduler;
    PairKey pair = key(30u);
    check(
        decide(scheduler, pair, 300u, 1u, 3000u)
            == ScheduleDecision::StageLeft,
        "mutation test pair did not stage");
    pair.poseSequence[1] += 1u;
    check(
        decide(scheduler, pair, 301u, 2u, 3001u)
            == ScheduleDecision::Rejected,
        "same pair ID with changed metadata was accepted");
    check(!scheduler.active(), "metadata rejection retained pending state");

    pair = key(31u);
    check(
        decide(scheduler, pair, 310u, 1u, 3100u)
            == ScheduleDecision::StageLeft,
        "pose-regression test pair did not stage");
    check(
        decide(scheduler, pair, 309u, 2u, 3101u)
            == ScheduleDecision::Rejected,
        "host pose regression was accepted");
    check(!scheduler.active(), "pose regression retained pending state");
}

void testEpochThreadAndInvalidInputReject()
{
    Scheduler scheduler;
    PairKey pair = key(40u);
    check(
        decide(scheduler, pair, 400u, 1u, 4000u)
            == ScheduleDecision::StageLeft,
        "epoch test pair did not stage");
    pair.poseProducerEpoch += 1u;
    check(
        decide(scheduler, pair, 401u, 2u, 4001u)
            == ScheduleDecision::Rejected,
        "same pair crossed a host pose epoch");

    pair = key(41u);
    check(
        decide(scheduler, pair, 410u, 1u, 4100u)
            == ScheduleDecision::StageLeft,
        "thread test pair did not stage");
    pair.d3dThreadId += 1u;
    check(
        decide(scheduler, pair, 411u, 2u, 4101u)
            == ScheduleDecision::Rejected,
        "same pair crossed a D3D thread");

    PairKey invalid {};
    check(
        decide(scheduler, invalid, 1u, 1u, 1u)
            == ScheduleDecision::Rejected,
        "invalid key was accepted");
    check(!scheduler.active(), "invalid key retained pending state");
}

void testBoundedAgeAndReplacementPoseRegression()
{
    Scheduler scheduler;
    const PairKey pair = key(45u);
    check(
        decide(scheduler, pair, 450u, 1u, 4500u)
            == ScheduleDecision::StageLeft,
        "age test pair did not stage");
    check(
        decide(scheduler, pair, 451u, 4000u, 4751u)
            == ScheduleDecision::Rejected,
        "staged pair exceeded the real-time age bound");
    check(!scheduler.active(), "age rejection retained pending state");

    check(
        decide(scheduler, key(46u), 460u, 5000u, 5000u)
            == ScheduleDecision::StageLeft,
        "replacement regression pair did not stage");
    check(
        decide(scheduler, key(47u), 459u, 5001u, 5001u)
            == ScheduleDecision::Rejected,
        "new pair bypassed the host-pose regression guard");
    check(
        !scheduler.active(),
        "replacement pose regression retained pending state");
}

void testExplicitCancellation()
{
    Scheduler scheduler;
    check(
        decide(scheduler, key(50u), 500u, 1u, 5000u)
            == ScheduleDecision::StageLeft,
        "cancel test pair did not stage");
    scheduler.cancel();
    check(!scheduler.active(), "explicit cancellation retained pending state");
    check(
        decide(scheduler, key(50u), 501u, 2u, 5001u)
            == ScheduleDecision::StageLeft,
        "cancelled pair could not start a clean stage");
}
}

int main()
{
    testFreshPoseGateAndAtomicCompletion();
    testNewPairReplacesUnpublishedStage();
    testSamePairMutationAndPoseRegressionReject();
    testEpochThreadAndInvalidInputReject();
    testBoundedAgeAndReplacementPoseRegression();
    testExplicitCancellation();
    if (failures != 0)
        return 1;

    std::cout
        << "CpuTemporalReadbackTest: PASS "
        << "freshPose=1 atomic=1 replace=1 metadata=1 "
        << "epoch=1 thread=1 boundedAge=1 cancel=1\n";
    return 0;
}
