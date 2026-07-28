#pragma once

#include <openxr/openxr.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace gtaiv_xr_host
{
namespace menu_quad_detail
{
inline bool finite(float value)
{
    return std::isfinite(value);
}

inline bool normalize(XrQuaternionf& value)
{
    const float lengthSquared =
        value.x * value.x
        + value.y * value.y
        + value.z * value.z
        + value.w * value.w;
    if (!finite(lengthSquared) || lengthSquared < 1.0e-8f)
        return false;
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    value.x *= inverseLength;
    value.y *= inverseLength;
    value.z *= inverseLength;
    value.w *= inverseLength;
    return true;
}

inline XrVector3f cross(
    const XrVector3f& left,
    const XrVector3f& right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x
    };
}

inline XrVector3f rotate(
    const XrQuaternionf& orientation,
    const XrVector3f& value)
{
    const XrVector3f imaginary {
        orientation.x,
        orientation.y,
        orientation.z
    };
    XrVector3f twiceCross = cross(imaginary, value);
    twiceCross.x *= 2.0f;
    twiceCross.y *= 2.0f;
    twiceCross.z *= 2.0f;
    const XrVector3f secondCross = cross(imaginary, twiceCross);
    return {
        value.x + orientation.w * twiceCross.x + secondCross.x,
        value.y + orientation.w * twiceCross.y + secondCross.y,
        value.z + orientation.w * twiceCross.z + secondCross.z
    };
}

inline bool buildPose(
    const std::array<XrView, 2u>& views,
    float distance,
    XrPosef& output)
{
    if (!finite(distance) || distance <= 0.0f)
        return false;
    XrQuaternionf orientation = views[0].pose.orientation;
    if (!normalize(orientation))
        return false;
    const XrVector3f midpoint {
        0.5f * (
            views[0].pose.position.x
            + views[1].pose.position.x),
        0.5f * (
            views[0].pose.position.y
            + views[1].pose.position.y),
        0.5f * (
            views[0].pose.position.z
            + views[1].pose.position.z)
    };
    if (!finite(midpoint.x)
        || !finite(midpoint.y)
        || !finite(midpoint.z))
    {
        return false;
    }
    const XrVector3f forward =
        rotate(orientation, { 0.0f, 0.0f, -1.0f });
    output = {};
    output.orientation = orientation;
    output.position = {
        midpoint.x + forward.x * distance,
        midpoint.y + forward.y * distance,
        midpoint.z + forward.z * distance
    };
    return finite(output.position.x)
        && finite(output.position.y)
        && finite(output.position.z);
}
}

class StationaryMenuQuadLatch
{
public:
    bool update(
        bool visible,
        uint32_t referenceSpaceGeneration,
        const std::array<XrView, 2u>& views,
        float distance)
    {
        if (!visible)
        {
            active_ = false;
            generation_ = 0u;
            pose_ = {};
            return false;
        }
        if (active_ && generation_ == referenceSpaceGeneration)
            return true;
        XrPosef candidate {};
        if (!menu_quad_detail::buildPose(views, distance, candidate))
            return false;
        pose_ = candidate;
        generation_ = referenceSpaceGeneration;
        active_ = true;
        return true;
    }

    bool active() const noexcept
    {
        return active_;
    }

    const XrPosef& pose() const noexcept
    {
        return pose_;
    }

private:
    XrPosef pose_ {};
    uint32_t generation_ = 0u;
    bool active_ = false;
};

inline bool StationaryMenuQuadPoseSelfTest(std::string& failure)
{
    std::array<XrView, 2u> views {
        XrView { XR_TYPE_VIEW },
        XrView { XR_TYPE_VIEW }
    };
    views[0].pose.orientation.w = 1.0f;
    views[1].pose.orientation.w = 1.0f;
    views[0].pose.position.x = -0.032f;
    views[1].pose.position.x = 0.032f;

    StationaryMenuQuadLatch latch;
    if (!latch.update(true, 1u, views, 1.8f))
    {
        failure = "stationary menu quad did not latch";
        return false;
    }
    const XrPosef first = latch.pose();
    constexpr float Epsilon = 1.0e-5f;
    if (std::fabs(first.position.x) > Epsilon
        || std::fabs(first.position.y) > Epsilon
        || std::fabs(first.position.z + 1.8f) > Epsilon
        || std::fabs(first.orientation.w - 1.0f) > Epsilon)
    {
        failure = "stationary menu quad identity pose is incorrect";
        return false;
    }

    views[0].pose.position.x = 10.0f;
    views[1].pose.position.x = 10.1f;
    views[0].pose.position.z = 4.0f;
    if (!latch.update(true, 1u, views, 1.8f)
        || std::fabs(latch.pose().position.x - first.position.x) > Epsilon
        || std::fabs(latch.pose().position.z - first.position.z) > Epsilon)
    {
        failure = "stationary menu quad followed the head after latching";
        return false;
    }

    latch.update(false, 1u, views, 1.8f);
    if (latch.active()
        || !latch.update(true, 1u, views, 1.8f)
        || std::fabs(latch.pose().position.x - first.position.x) < 1.0f)
    {
        failure = "stationary menu quad did not relatch after closing";
        return false;
    }
    failure.clear();
    return true;
}
}
