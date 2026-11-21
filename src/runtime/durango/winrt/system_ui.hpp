#pragma once
#include <cstdint>
#include <string>
#include <string_view>

struct IInspectable;
typedef struct HSTRING__ *HSTRING;

namespace xv::runtime::durango::winrt {
// "gamertag", "xbox_user_id", "audio_device_id"; "gamerscore", "age_group"
std::wstring console_string(std::wstring_view key);
std::uint32_t console_number(std::wstring_view key);
void *audio_devices();
// IAsyncOperation<HSTRING> finished when the host's window closes
void *show_keyboard(HSTRING text, HSTRING title, HSTRING description, std::uint32_t scope);
void *show_keyboard_with_options(void *options);
// Windows.Xbox.UI.KeyboardOptions
IInspectable *system_ui_class(std::wstring_view name);
} // namespace xv::runtime::durango::winrt
