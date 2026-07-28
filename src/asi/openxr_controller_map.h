#pragma once

#include "../bridge/gtaiv_xr_pose_bridge.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace asi::controller_map
{
inline uint16_t motorLow(uint32_t packed)
{
    return static_cast<uint16_t>(packed & 0xffffu);
}

inline uint16_t motorHigh(uint32_t packed)
{
    return static_cast<uint16_t>((packed >> 16u) & 0xffffu);
}

inline uint32_t packMotors(uint16_t left, uint16_t right)
{
    return static_cast<uint32_t>(left)
        | (static_cast<uint32_t>(right) << 16u);
}

inline bool controllerInputAvailable(
    const gtaiv_xr_bridge::PoseBridge& pose,
    uint32_t hand)
{
    const uint32_t flag = hand == 0u
        ? gtaiv_xr_bridge::PoseBridgeLeftControllerValid
        : gtaiv_xr_bridge::PoseBridgeRightControllerValid;
    return (pose.flags & flag) != 0u;
}

inline bool pressed(
    const gtaiv_xr_bridge::ControllerState& controller,
    uint32_t button)
{
    return (controller.buttons & button) != 0u;
}

inline SHORT stickValue(float value)
{
    const float clamped =
        (std::max)(-1.0f, (std::min)(value, 1.0f));
    if (clamped <= -1.0f)
        return static_cast<SHORT>(-32768);
    return static_cast<SHORT>(
        std::lround(clamped * 32767.0f));
}

inline BYTE triggerValue(float value)
{
    return static_cast<BYTE>(std::lround(
        (std::max)(0.0f, (std::min)(value, 1.0f))
        * 255.0f));
}

inline bool synthesizeGamepad(
    const gtaiv_xr_bridge::PoseBridge& pose,
    XINPUT_GAMEPAD& gamepad)
{
    gamepad = {};
    const auto& left = pose.controllers[0];
    const auto& right = pose.controllers[1];
    const bool haveLeft = controllerInputAvailable(pose, 0u);
    const bool haveRight = controllerInputAvailable(pose, 1u);
    if (!haveLeft && !haveRight)
        return false;

    if (haveLeft)
    {
        gamepad.sThumbLX = stickValue(left.thumbstickX);
        gamepad.sThumbLY = stickValue(left.thumbstickY);
        gamepad.bLeftTrigger = triggerValue(left.trigger);
        if (pressed(
                left,
                gtaiv_xr_bridge::ControllerPrimaryClick))
        {
            gamepad.wButtons |= XINPUT_GAMEPAD_X;
        }
        if (pressed(
                left,
                gtaiv_xr_bridge::ControllerSecondaryClick))
        {
            gamepad.wButtons |= XINPUT_GAMEPAD_Y;
        }
        if (pressed(
                left,
                gtaiv_xr_bridge::ControllerSqueezePressed))
        {
            gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        }
    }
    if (haveRight)
    {
        gamepad.sThumbRX = stickValue(right.thumbstickX);
        gamepad.sThumbRY = stickValue(right.thumbstickY);
        gamepad.bRightTrigger = triggerValue(right.trigger);
        if (pressed(
                right,
                gtaiv_xr_bridge::ControllerPrimaryClick))
        {
            gamepad.wButtons |= XINPUT_GAMEPAD_A;
        }
        if (pressed(
                right,
                gtaiv_xr_bridge::ControllerSecondaryClick))
        {
            gamepad.wButtons |= XINPUT_GAMEPAD_B;
        }
        if (pressed(
                right,
                gtaiv_xr_bridge::ControllerSqueezePressed))
        {
            gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        }
    }

    const bool leftThumb =
        haveLeft
        && pressed(
            left,
            gtaiv_xr_bridge::ControllerThumbstickPressed);
    const bool rightThumb =
        haveRight
        && pressed(
            right,
            gtaiv_xr_bridge::ControllerThumbstickPressed);
    const bool menu =
        haveLeft
        && pressed(
            left,
            gtaiv_xr_bridge::ControllerMenuClick);

    // Touch has no D-pad or Back button. L3 + left-stick supplies the D-pad;
    // Menu + L3 supplies Back. Menu + R3 remains the host recenter chord.
    constexpr float DpadThreshold = 0.55f;
    const bool dpadChord =
        leftThumb
        && (std::fabs(left.thumbstickX) >= DpadThreshold
            || std::fabs(left.thumbstickY) >= DpadThreshold);
    if (dpadChord)
    {
        if (left.thumbstickX <= -DpadThreshold)
            gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (left.thumbstickX >= DpadThreshold)
            gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (left.thumbstickY <= -DpadThreshold)
            gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (left.thumbstickY >= DpadThreshold)
            gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    }
    else if (leftThumb && !menu)
    {
        gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    }

    if (rightThumb && !menu)
        gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (menu && leftThumb)
        gamepad.wButtons |= XINPUT_GAMEPAD_BACK;
    else if (menu && !rightThumb)
        gamepad.wButtons |= XINPUT_GAMEPAD_START;
    return true;
}
}
