#pragma once

#include "gtaiv_xr_pose_bridge.h"

#include <algorithm>
#include <cmath>

namespace gtaiv_xr_bridge
{
inline bool ComputeOpenXrCoverTangents(
    const EyeFov eyeFovs[2],
    float* horizontal,
    float* vertical)
{
    if (!eyeFovs || !horizontal || !vertical)
        return false;

    float coverHorizontal = 0.0f;
    float coverVertical = 0.0f;
    for (uint32_t eye = 0u; eye < 2u; ++eye)
    {
        const EyeFov& fov = eyeFovs[eye];
        if (!std::isfinite(fov.angleLeft) ||
            !std::isfinite(fov.angleRight) ||
            !std::isfinite(fov.angleUp) ||
            !std::isfinite(fov.angleDown) ||
            !(fov.angleLeft < 0.0f) ||
            !(fov.angleRight > 0.0f) ||
            !(fov.angleUp > 0.0f) ||
            !(fov.angleDown < 0.0f))
        {
            return false;
        }

        const float left = -std::tan(fov.angleLeft);
        const float right = std::tan(fov.angleRight);
        const float up = std::tan(fov.angleUp);
        const float down = -std::tan(fov.angleDown);
        if (!std::isfinite(left) ||
            !std::isfinite(right) ||
            !std::isfinite(up) ||
            !std::isfinite(down))
        {
            return false;
        }
        coverHorizontal =
            (std::max)(coverHorizontal, (std::max)(left, right));
        coverVertical =
            (std::max)(coverVertical, (std::max)(up, down));
    }

    if (!(coverHorizontal > 0.05f) ||
        !(coverVertical > 0.05f) ||
        !(coverHorizontal < 5.0f) ||
        !(coverVertical < 5.0f))
    {
        return false;
    }

    *horizontal = coverHorizontal;
    *vertical = coverVertical;
    return true;
}
}
