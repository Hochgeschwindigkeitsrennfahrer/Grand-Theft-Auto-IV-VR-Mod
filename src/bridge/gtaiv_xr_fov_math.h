#pragma once

#include "gtaiv_xr_pose_bridge.h"

#include <algorithm>
#include <cmath>

namespace gtaiv_xr_bridge
{
struct FrustumCanvasMapping
{
    float sourceLeft;
    float sourceRight;
    float sourceTop;
    float sourceBottom;
    float destinationLeft;
    float destinationRight;
    float destinationTop;
    float destinationBottom;
};

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

// Compute normalized source/destination rectangles for packing a symmetric
// game frustum into one asymmetric runtime eye frustum. When the published
// game tangents equal the two-eye cover tangents, every eye destination is
// full-size and only the source is cropped to that eye's asymmetric view.
inline bool ComputeFrustumCanvasMapping(
    float gameHorizontal,
    float gameVertical,
    float eyeLeft,
    float eyeRight,
    float eyeTop,
    float eyeBottom,
    FrustumCanvasMapping* output)
{
    if (!output ||
        !std::isfinite(gameHorizontal) ||
        !std::isfinite(gameVertical) ||
        !std::isfinite(eyeLeft) ||
        !std::isfinite(eyeRight) ||
        !std::isfinite(eyeTop) ||
        !std::isfinite(eyeBottom) ||
        !(gameHorizontal > 0.05f) ||
        !(gameVertical > 0.05f) ||
        // Mode58's verified CCam=110 raster is about tanV=3.03.
        // A real 16:9 backbuffer therefore has tanH≈5.38; rejecting at 5
        // silently turned a valid wide source into the old framed fallback.
        !(gameHorizontal < 16.0f) ||
        !(gameVertical < 16.0f) ||
        !(eyeRight > eyeLeft) ||
        !(eyeBottom > eyeTop))
    {
        return false;
    }

    const float tangentLeft =
        (std::max)(-gameHorizontal, eyeLeft);
    const float tangentRight =
        (std::min)(gameHorizontal, eyeRight);
    const float tangentTop =
        (std::max)(-gameVertical, eyeTop);
    const float tangentBottom =
        (std::min)(gameVertical, eyeBottom);
    if (tangentRight - tangentLeft < 0.05f ||
        tangentBottom - tangentTop < 0.05f)
    {
        return false;
    }

    FrustumCanvasMapping mapping {};
    mapping.sourceLeft =
        (tangentLeft + gameHorizontal) /
        (2.0f * gameHorizontal);
    mapping.sourceRight =
        (tangentRight + gameHorizontal) /
        (2.0f * gameHorizontal);
    mapping.sourceTop =
        (tangentTop + gameVertical) /
        (2.0f * gameVertical);
    mapping.sourceBottom =
        (tangentBottom + gameVertical) /
        (2.0f * gameVertical);
    mapping.destinationLeft =
        (tangentLeft - eyeLeft) / (eyeRight - eyeLeft);
    mapping.destinationRight =
        (tangentRight - eyeLeft) / (eyeRight - eyeLeft);
    mapping.destinationTop =
        (tangentTop - eyeTop) / (eyeBottom - eyeTop);
    mapping.destinationBottom =
        (tangentBottom - eyeTop) / (eyeBottom - eyeTop);

    const auto inUnitRange = [](float value) {
        return std::isfinite(value) &&
            value >= -1.0e-4f &&
            value <= 1.0001f;
    };
    if (!inUnitRange(mapping.sourceLeft) ||
        !inUnitRange(mapping.sourceRight) ||
        !inUnitRange(mapping.sourceTop) ||
        !inUnitRange(mapping.sourceBottom) ||
        !inUnitRange(mapping.destinationLeft) ||
        !inUnitRange(mapping.destinationRight) ||
        !inUnitRange(mapping.destinationTop) ||
        !inUnitRange(mapping.destinationBottom) ||
        !(mapping.sourceRight > mapping.sourceLeft) ||
        !(mapping.sourceBottom > mapping.sourceTop) ||
        !(mapping.destinationRight > mapping.destinationLeft) ||
        !(mapping.destinationBottom > mapping.destinationTop))
    {
        return false;
    }

    *output = mapping;
    return true;
}
}
