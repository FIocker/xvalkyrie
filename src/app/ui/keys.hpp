#pragma once

namespace xs::ui {
// GLFW key -> Windows virtual-key code, which is what input bindings are in. 0 if none.
int virtual_key(int glfw_key);
} // namespace xs::ui
