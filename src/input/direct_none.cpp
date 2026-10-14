#include "input/input.hpp"

// No direct capture on this host: poll() falls back to what the window sees.
namespace xs::input::detail {
bool direct_available() { return false; }
bool direct_key(int) { return false; }
bool direct_gamepad(GamepadState &) { return false; }
} // namespace xs::input::detail
