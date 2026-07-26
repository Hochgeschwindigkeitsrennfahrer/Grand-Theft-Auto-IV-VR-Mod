#include "pose_bridge.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <sstream>
#include <utility>

namespace gtaiv_xr_host
{
namespace
{
using gtaiv_xr_bridge::PoseBridge;

static_assert(sizeof(LONG) == sizeof(int32_t), "Windows LONG must remain 32-bit");

LONG bitsToLong(uint32_t bits)
{
    LONG value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t longToBits(LONG value)
{
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
}

struct PoseBridgePublisher::Implementation
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

    bool initialize()
    {
        producerMutex = CreateMutexW(
            nullptr,
            FALSE,
            gtaiv_xr_bridge::PoseProducerMutexName);
        if (!producerMutex)
        {
            log("PoseBridge: CreateMutex failed");
            reset();
            return false;
        }

        const DWORD mutexResult = WaitForSingleObject(producerMutex, 0u);
        if (mutexResult != WAIT_OBJECT_0 && mutexResult != WAIT_ABANDONED)
        {
            log("PoseBridge: another x64 host is already publishing poses");
            reset();
            return false;
        }
        ownsMutex = true;

        mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0u,
            static_cast<DWORD>(sizeof(PoseBridge)),
            gtaiv_xr_bridge::PoseMappingName);
        if (!mapping)
        {
            log("PoseBridge: CreateFileMapping failed");
            reset();
            return false;
        }

        shared = static_cast<PoseBridge*>(MapViewOfFile(
            mapping,
            FILE_MAP_READ | FILE_MAP_WRITE,
            0u,
            0u,
            sizeof(PoseBridge)));
        if (!shared)
        {
            log("PoseBridge: MapViewOfFile failed");
            reset();
            return false;
        }

        LARGE_INTEGER counter {};
        QueryPerformanceCounter(&counter);
        producerEpoch = static_cast<uint64_t>(counter.QuadPart)
            ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32u)
            ^ GetTickCount64();
        if (producerEpoch == 0u)
            producerEpoch = 1u;

        PoseBridge stopped {};
        stopped.magic = gtaiv_xr_bridge::PoseMagic;
        stopped.version = gtaiv_xr_bridge::PoseVersion;
        stopped.structBytes = sizeof(PoseBridge);
        stopped.producerPid = GetCurrentProcessId();
        stopped.producerEpoch = producerEpoch;
        stopped.heartbeatTickMs = GetTickCount64();
        commit(stopped);

        std::ostringstream message;
        message << "PoseBridge: READY producer=x64 epoch=" << producerEpoch;
        log(message.str());
        return true;
    }

    void publish(PoseBridge value)
    {
        if (!shared)
            return;

        value.magic = gtaiv_xr_bridge::PoseMagic;
        value.version = gtaiv_xr_bridge::PoseVersion;
        value.structBytes = sizeof(PoseBridge);
        value.producerPid = GetCurrentProcessId();
        value.producerEpoch = producerEpoch;
        value.heartbeatTickMs = GetTickCount64();
        commit(value);
    }

    void reset()
    {
        if (shared)
        {
            PoseBridge stopped {};
            stopped.magic = gtaiv_xr_bridge::PoseMagic;
            stopped.version = gtaiv_xr_bridge::PoseVersion;
            stopped.structBytes = sizeof(PoseBridge);
            stopped.producerPid = GetCurrentProcessId();
            stopped.producerEpoch = producerEpoch;
            stopped.heartbeatTickMs = GetTickCount64();
            commit(stopped);
            UnmapViewOfFile(shared);
            shared = nullptr;
        }
        if (mapping)
        {
            CloseHandle(mapping);
            mapping = nullptr;
        }
        if (ownsMutex && producerMutex)
            ReleaseMutex(producerMutex);
        ownsMutex = false;
        if (producerMutex)
        {
            CloseHandle(producerMutex);
            producerMutex = nullptr;
        }
        producerEpoch = 0u;
    }

    void commit(const PoseBridge& value)
    {
        if (!shared)
            return;

        const uint32_t previousBits = longToBits(
            *reinterpret_cast<volatile LONG*>(&shared->publicationSequence));
        const uint32_t stableBits = (previousBits & 1u) != 0u
            ? previousBits + 1u
            : previousBits;
        uint32_t writingBits = stableBits + 1u;
        if (writingBits == 0u)
            writingBits = 1u;
        uint32_t publishedBits = writingBits + 1u;
        if (publishedBits == 0u)
            publishedBits = 2u;

        volatile LONG* const sequence =
            reinterpret_cast<volatile LONG*>(&shared->publicationSequence);
        InterlockedExchange(sequence, bitsToLong(writingBits));
        MemoryBarrier();

        constexpr size_t prefixBytes = offsetof(PoseBridge, publicationSequence);
        constexpr size_t suffixOffset =
            offsetof(PoseBridge, publicationSequence) + sizeof(int32_t);
        std::memcpy(shared, &value, prefixBytes);
        std::memcpy(
            reinterpret_cast<unsigned char*>(shared) + suffixOffset,
            reinterpret_cast<const unsigned char*>(&value) + suffixOffset,
            sizeof(PoseBridge) - suffixOffset);

        MemoryBarrier();
        InterlockedExchange(sequence, bitsToLong(publishedBits));
    }

    LogFunction logger;
    HANDLE producerMutex = nullptr;
    HANDLE mapping = nullptr;
    PoseBridge* shared = nullptr;
    uint64_t producerEpoch = 0u;
    bool ownsMutex = false;
};

PoseBridgePublisher::PoseBridgePublisher() = default;

PoseBridgePublisher::~PoseBridgePublisher()
{
    reset();
}

bool PoseBridgePublisher::initialize(LogFunction logger)
{
    reset();
    implementation_ = new (std::nothrow) Implementation(std::move(logger));
    if (!implementation_)
        return false;
    if (!implementation_->initialize())
    {
        reset();
        return false;
    }
    return true;
}

void PoseBridgePublisher::publish(PoseBridge value)
{
    if (implementation_)
        implementation_->publish(value);
}

void PoseBridgePublisher::reset()
{
    delete implementation_;
    implementation_ = nullptr;
}
}
