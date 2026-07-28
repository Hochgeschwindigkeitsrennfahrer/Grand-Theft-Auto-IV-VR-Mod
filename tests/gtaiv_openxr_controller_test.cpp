#include "../src/asi/openxr_controller_map.h"

#include <iostream>

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

bool has(const XINPUT_GAMEPAD& gamepad, WORD button)
{
    return (gamepad.wButtons & button) != 0u;
}
}

int main()
{
    using namespace gtaiv_xr_bridge;
    PoseBridge pose {};
    XINPUT_GAMEPAD gamepad {};
    check(
        !asi::controller_map::synthesizeGamepad(pose, gamepad),
        "controller-less pose was accepted");

    pose.flags =
        PoseBridgeLeftControllerValid
        | PoseBridgeRightControllerValid;
    pose.controllers[0].thumbstickX = -1.0f;
    pose.controllers[0].thumbstickY = 0.5f;
    pose.controllers[0].trigger = 0.25f;
    pose.controllers[0].buttons =
        ControllerPrimaryClick
        | ControllerSecondaryClick
        | ControllerSqueezePressed;
    pose.controllers[1].thumbstickX = 1.0f;
    pose.controllers[1].thumbstickY = -0.5f;
    pose.controllers[1].trigger = 0.75f;
    pose.controllers[1].buttons =
        ControllerPrimaryClick
        | ControllerSecondaryClick
        | ControllerSqueezePressed;
    check(
        asi::controller_map::synthesizeGamepad(pose, gamepad),
        "two-controller pose was rejected");
    check(
        gamepad.sThumbLX == -32768
            && gamepad.sThumbLY == 16384
            && gamepad.sThumbRX == 32767
            && gamepad.sThumbRY == -16384,
        "stick mapping or rounding is incorrect");
    check(
        gamepad.bLeftTrigger == 64u
            && gamepad.bRightTrigger == 191u,
        "trigger mapping or rounding is incorrect");
    check(
        has(gamepad, XINPUT_GAMEPAD_X)
            && has(gamepad, XINPUT_GAMEPAD_Y)
            && has(gamepad, XINPUT_GAMEPAD_A)
            && has(gamepad, XINPUT_GAMEPAD_B)
            && has(gamepad, XINPUT_GAMEPAD_LEFT_SHOULDER)
            && has(gamepad, XINPUT_GAMEPAD_RIGHT_SHOULDER),
        "face or squeeze button mapping is incomplete");

    pose.controllers[0] = {};
    pose.controllers[1] = {};
    pose.controllers[0].buttons = ControllerThumbstickPressed;
    check(
        asi::controller_map::synthesizeGamepad(pose, gamepad)
            && has(gamepad, XINPUT_GAMEPAD_LEFT_THUMB),
        "left thumb click is missing");
    pose.controllers[0].thumbstickX = 0.8f;
    pose.controllers[0].thumbstickY = -0.8f;
    check(
        asi::controller_map::synthesizeGamepad(pose, gamepad)
            && has(gamepad, XINPUT_GAMEPAD_DPAD_RIGHT)
            && has(gamepad, XINPUT_GAMEPAD_DPAD_DOWN)
            && !has(gamepad, XINPUT_GAMEPAD_LEFT_THUMB),
        "left-thumb D-pad chord is incorrect");

    pose.controllers[0] = {};
    pose.controllers[1] = {};
    pose.controllers[0].buttons = ControllerMenuClick;
    check(
        asi::controller_map::synthesizeGamepad(pose, gamepad)
            && has(gamepad, XINPUT_GAMEPAD_START),
        "Menu-to-Start mapping is missing");
    pose.controllers[0].buttons =
        ControllerMenuClick | ControllerThumbstickPressed;
    check(
        asi::controller_map::synthesizeGamepad(pose, gamepad)
            && has(gamepad, XINPUT_GAMEPAD_BACK)
            && !has(gamepad, XINPUT_GAMEPAD_START),
        "Menu+L3 Back chord is incorrect");
    pose.controllers[0].buttons = ControllerMenuClick;
    pose.controllers[1].buttons = ControllerThumbstickPressed;
    check(
        asi::controller_map::synthesizeGamepad(pose, gamepad)
            && !has(gamepad, XINPUT_GAMEPAD_START)
            && !has(gamepad, XINPUT_GAMEPAD_RIGHT_THUMB),
        "Menu+R3 recenter chord leaked into GTA input");

    const uint32_t motors =
        asi::controller_map::packMotors(0x1234u, 0xabcdu);
    check(
        asi::controller_map::motorLow(motors) == 0x1234u
            && asi::controller_map::motorHigh(motors) == 0xabcdu,
        "XInput-to-Touch haptic motor routing is incorrect");

    if (failures != 0)
        return 1;
    std::cout
        << "OpenXrControllerTest: PASS "
        << "sticks=1 triggers=1 face=1 shoulders=1 "
        << "thumbs=1 dpad=1 menu=1 recenter=1 haptics=1\n";
    return 0;
}
