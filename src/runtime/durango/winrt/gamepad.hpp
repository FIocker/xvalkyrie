#pragma once
#include <string_view>

struct IInspectable;

namespace xs::runtime::durango::winrt {
// Activation factory of Windows.Xbox.Input.Gamepad, null for other names. There is one
// gamepad, always connected, whose readings come from xs::input.
IInspectable *input_class(std::wstring_view name);
// An IVectorView of the one gamepad, with a reference to it.
void *gamepad_list();
// The same gamepad as the IController that Controller::Controllers lists.
void *controller_list();
} // namespace xs::runtime::durango::winrt
