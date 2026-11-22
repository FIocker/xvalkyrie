#pragma once
#include <string>

namespace xv::runtime {
class Registry;
namespace modules {
// The console's language and region as a locale name: the one of Options > Console, the
// host user's if none is set there or the host does not know it.
const std::wstring &console_language();
// The Win32 functions that tell a title the user's language, answered with it
void install_locale(Registry &registry);
} // namespace modules
} // namespace xv::runtime
