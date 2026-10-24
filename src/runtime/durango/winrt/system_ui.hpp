#pragma once
#include <cstdint>
#include <string>
#include <string_view>

struct IInspectable;
typedef struct HSTRING__ *HSTRING;

namespace xs::runtime::durango::winrt {
// What runtime::console_settings() says, by name: "gamertag", "xbox_user_id",
// "audio_device_id" (the host's communications output), and the numbers "gamerscore" and
// "age_group".
std::wstring console_string(std::wstring_view key);
std::uint32_t console_number(std::wstring_view key);
// User.AudioDevices: the headset of Options > Console, if there is one
void *audio_devices();
// SystemUI.ShowVirtualKeyboardAsync(default text, title, description, input scope): an
// IAsyncOperation<HSTRING> that finishes when the host's window is closed
void *show_keyboard(HSTRING text, HSTRING title, HSTRING description, std::uint32_t scope);
// SystemUI.ShowVirtualKeyboardWithOptionsAsync(KeyboardOptions)
void *show_keyboard_with_options(void *options);
// Windows.Xbox.UI.KeyboardOptions, a class of ours; null for any other name
IInspectable *system_ui_class(std::wstring_view name);
} // namespace xs::runtime::durango::winrt
