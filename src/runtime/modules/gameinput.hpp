#pragma once

namespace xs::runtime {
class Registry;
namespace modules {
// GameInput.dll: one gamepad, the emulated one
void install_gameinput(Registry &registry);
} // namespace modules
} // namespace xs::runtime
