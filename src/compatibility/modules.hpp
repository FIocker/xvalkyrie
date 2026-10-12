#pragma once
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace xs::compat {
struct ModuleClass {
    std::string_view category; // "api_set", "d3d_scarlett", "game_os_only", "unknown", ...
    bool os_surface;           // system DLL, its imports get checked against the API list
    bool development_only;
};
// Same categories as Microsoft's xbdepends tool. Case-insensitive, whole name.
ModuleClass classify_module(std::string_view file_name);
// is this function in the list of OS APIs console titles may use? (case-sensitive)
bool is_game_os_api(std::string_view symbol);
// Categories of all imported DLLs and the target they point to (scarlett, xbox_one, pc...)
nlohmann::json import_profile(const nlohmann::json &pe_report);
} // namespace xs::compat
