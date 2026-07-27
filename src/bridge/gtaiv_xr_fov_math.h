#pragma once

#include "gtaiv_xr_pose_bridge.h"

#include <algorithm>
#include <cmath>

namespace gtaiv_xr_bridge
{
inline bool ComputeOpenXrEyeRawTangents(
    const EyeFov eyeFovs[2],
    uint32_t eye,
    float* left,
    float* right,
    float* top,
    float* bottom)
{
    if (!eyeFovs || eye >= 2u || !left || !right || !top || !bottom)
        return false;

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

    // OpenVR's legacy canvas code uses negative left/top and positive
    // right/bottom tangents. Convert the OpenXR angular FOV to that local
    // convention without touching the OpenVR runtime.
    const float rawLeft = std::tan(fov.angleLeft);
    const float rawRight = std::tan(fov.angleRight);
    const float rawTop = -std::tan(fov.angleUp);
    const float rawBottom = -std::tan(fov.angleDown);
    if (!std::isfinite(rawLeft) ||
        !std::isfinite(rawRight) ||
        !std::isfinite(rawTop) ||
        !std::isfinite(rawBottom) ||
        !(rawLeft < -0.05f) ||
        !(rawRight > 0.05f) ||
        !(rawTop < -0.05f) ||
        !(rawBottom > 0.05f) ||
        !(rawLeft > -5.0f) ||
        !(rawRight < 5.0f) ||
        !(rawTop > -5.0f) ||
        !(rawBottom < 5.0f))
    {
        return false;
    }

    *left = rawLeft;
    *right = rawRight;
    *top = rawTop;
    *bottom = rawBottom;
    return true;
}

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
        float rawLeft = 0.0f;
        float rawRight = 0.0f;
        float rawTop = 0.0f;
        float rawBottom = 0.0f;
        if (!ComputeOpenXrEyeRawTangents(
                eyeFovs,
                eye,
                &rawLeft,
                &rawRight,
                &rawTop,
                &rawBottom))
            return false;

        coverHorizontal =
            (std::max)(coverHorizontal, (std::max)(-rawLeft, rawRight));
        coverVertical =
            (std::max)(coverVertical, (std::max)(-rawTop, rawBottom));
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
