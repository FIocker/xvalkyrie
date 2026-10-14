#pragma once
#include <string_view>

struct IInspectable;

namespace xs::runtime::durango::winrt {
// Activation factory of Windows.Xbox.Input.Gamepad, null for other names. There is one
// gamepad, always connected, whose readings come from xs::input.
IInspectable *input_class(std::wstring_view name);
} // namespace xs::runtime::durango::winrt
