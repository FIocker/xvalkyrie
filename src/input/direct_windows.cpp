#include "input/input.hpp"
#include <algorithm>
#include <windows.h>
#include <xinput.h>

// Direct capture on Windows: the keyboard through GetAsyncKeyState and the first gamepad
// through XInput. Neither needs a window or a message loop. Both only count while a
// window of this process is in the foreground, so that typing elsewhere does not play.
namespace xs::input::detail {
namespace {
bool foreground() {
    DWORD process = 0;
    const auto window = GetForegroundWindow();
    return window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId();
}
float stick(SHORT value, SHORT dead_zone) {
    if (value > -dead_zone && value < dead_zone)
        return 0;
    return std::clamp(static_cast<float>(value) / 32767.f, -1.f, 1.f);
}
} // namespace

bool direct_available() { return true; }
bool direct_key(int key) { return foreground() && (GetAsyncKeyState(key) & 0x8000) != 0; }
bool direct_gamepad(GamepadState &state) {
    XINPUT_STATE pad{};
    if (!foreground() || XInputGetState(0, &pad) != ERROR_SUCCESS)
        return false;
    const auto &g = pad.Gamepad;
    const auto on = [&](WORD bit, Button button) {
        if (g.wButtons & bit)
            state.buttons |= button;
    };
    on(XINPUT_GAMEPAD_A, a);
    on(XINPUT_GAMEPAD_B, b);
    on(XINPUT_GAMEPAD_X, x);
    on(XINPUT_GAMEPAD_Y, y);
    on(XINPUT_GAMEPAD_START, menu);
    on(XINPUT_GAMEPAD_BACK, view);
    on(XINPUT_GAMEPAD_DPAD_UP, dpad_up);
    on(XINPUT_GAMEPAD_DPAD_DOWN, dpad_down);
    on(XINPUT_GAMEPAD_DPAD_LEFT, dpad_left);
    on(XINPUT_GAMEPAD_DPAD_RIGHT, dpad_right);
    on(XINPUT_GAMEPAD_LEFT_SHOULDER, left_shoulder);
    on(XINPUT_GAMEPAD_RIGHT_SHOULDER, right_shoulder);
    on(XINPUT_GAMEPAD_LEFT_THUMB, left_thumbstick);
    on(XINPUT_GAMEPAD_RIGHT_THUMB, right_thumbstick);
    state.left_trigger = g.bLeftTrigger / 255.f;
    state.right_trigger = g.bRightTrigger / 255.f;
    state.left_x = stick(g.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    state.left_y = stick(g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    state.right_x = stick(g.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    state.right_y = stick(g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    return true;
}
} // namespace xs::input::detail
