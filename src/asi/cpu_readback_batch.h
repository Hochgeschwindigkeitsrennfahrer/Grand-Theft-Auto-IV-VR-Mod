#pragma once

#include <cstdint>

namespace asi
{
namespace cpu_readback_batch
{
// GetRenderTargetData queues a GPU-to-CPU copy in DXVK. Queue every eye before
// the first LockRect so one blocking wait can cover the complete stereo batch.
template <typename QueueEye, typename AllQueued, typename LockEye>
bool queueAllBeforeLock(
    uint32_t eyeCount,
    QueueEye queueEye,
    AllQueued allQueued,
    LockEye lockEye)
{
    if (eyeCount == 0u)
        return false;

    for (uint32_t eye = 0u; eye < eyeCount; ++eye)
    {
        if (!queueEye(eye))
            return false;
    }

    allQueued();

    for (uint32_t eye = 0u; eye < eyeCount; ++eye)
    {
        if (!lockEye(eye))
            return false;
    }
    return true;
}
}
}
