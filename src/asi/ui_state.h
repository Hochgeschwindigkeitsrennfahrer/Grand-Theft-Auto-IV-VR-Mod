#pragma once

#include <cstdint>

namespace asi
{
struct UiPresentationState
{
    uint32_t reasonFlags = 0u;

    explicit operator bool() const noexcept
    {
        return reasonFlags != 0u;
    }
};

// Resolves read-only GTA IV CE state bytes for pause/map, loading, and phone.
// All three are required; OpenXR presentation remains fail-closed otherwise.
bool InstallUiStateProbe();
UiPresentationState GetUiPresentationState();
}
