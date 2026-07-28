#include "../src/asi/cpu_readback_batch.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testSuccessfulStereoBatch()
{
    std::vector<uint32_t> order;
    const bool ready = asi::cpu_readback_batch::queueAllBeforeLock(
        2u,
        [&](uint32_t eye) {
            order.push_back(10u + eye);
            return true;
        },
        [&]() {
            order.push_back(20u);
        },
        [&](uint32_t eye) {
            order.push_back(30u + eye);
            return true;
        });
    const std::vector<uint32_t> expected {10u, 11u, 20u, 30u, 31u};
    check(ready, "successful stereo batch was rejected");
    check(
        order == expected,
        "an eye was locked before every readback had been queued");
}

void testQueueFailureStopsBeforeLocks()
{
    std::vector<uint32_t> order;
    const bool ready = asi::cpu_readback_batch::queueAllBeforeLock(
        2u,
        [&](uint32_t eye) {
            order.push_back(10u + eye);
            return eye == 0u;
        },
        [&]() {
            order.push_back(20u);
        },
        [&](uint32_t eye) {
            order.push_back(30u + eye);
            return true;
        });
    const std::vector<uint32_t> expected {10u, 11u};
    check(!ready, "failed second readback queue was accepted");
    check(
        order == expected,
        "queue failure reached the lock phase");
}

void testLockFailureKeepsQueueFirstOrdering()
{
    std::vector<uint32_t> order;
    const bool ready = asi::cpu_readback_batch::queueAllBeforeLock(
        2u,
        [&](uint32_t eye) {
            order.push_back(10u + eye);
            return true;
        },
        [&]() {
            order.push_back(20u);
        },
        [&](uint32_t eye) {
            order.push_back(30u + eye);
            return eye == 0u;
        });
    const std::vector<uint32_t> expected {10u, 11u, 20u, 30u, 31u};
    check(!ready, "failed second lock was accepted");
    check(
        order == expected,
        "lock failure changed the queue-before-lock ordering");
}

void testEmptyBatchIsRejected()
{
    uint32_t calls = 0u;
    const bool ready = asi::cpu_readback_batch::queueAllBeforeLock(
        0u,
        [&](uint32_t) {
            ++calls;
            return true;
        },
        [&]() {
            ++calls;
        },
        [&](uint32_t) {
            ++calls;
            return true;
        });
    check(!ready, "empty readback batch was accepted");
    check(calls == 0u, "empty readback batch invoked a callback");
}
}

int main()
{
    testSuccessfulStereoBatch();
    testQueueFailureStopsBeforeLocks();
    testLockFailureKeepsQueueFirstOrdering();
    testEmptyBatchIsRejected();
    if (failures != 0)
        return 1;

    std::cout
        << "CpuReadbackBatchTest: PASS "
        << "queueAllBeforeLock=1 queueFailure=1 lockFailure=1 empty=1\n";
    return 0;
}
