#pragma once
#include <cstdint>

namespace xv::runtime {
class Registry;
namespace modules {
// APP_LOCAL_DEVICE_ID of the gamepad
inline constexpr std::uint8_t gamepad_device_id[32] = {0x58, 0x53, 1};
// the title has been told which user the gamepad is of
void gamepad_associated();
// GameInput.dll: one gamepad, the emulated one
void install_gameinput(Registry &registry);
} // namespace modules
} // namespace xv::runtime
