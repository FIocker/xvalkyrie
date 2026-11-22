#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xv {
struct LibrarySettings {
    std::vector<std::filesystem::path> directories;
    bool grid = true;
};
// UTF-8
struct ConsoleSettings {
    std::string gamertag = "Player";
    std::string xbox_user_id = "2814600000000001";
    std::uint32_t gamerscore = 0;
    std::uint32_t age_group = 3; // unknown, child, teen, adult
    bool headset = false;
    bool keyboard_dialog = true;
    // The folder with the console's own apps, each unpacked in a folder of its own (the
    // dashboard, its frameworks and the rest of the system's packages). Optional: with it,
    // an app finds the packages it depends on there, and the dashboard can be started.
    std::filesystem::path system_storage;
};
// The console's dashboard in the system storage: the folder of its package, empty if the
// storage is not set or has none.
std::filesystem::path system_menu(const ConsoleSettings &settings);
bool valid_gamertag(const std::string &gamertag);
bool valid_xbox_user_id(const std::string &id);
// config.ini, logs/, saves/ and cache/ live in the per-user configuration folder.
std::filesystem::path config_directory();
std::filesystem::path config_file();
// what can be worked out again but takes a while, such as the game library
std::filesystem::path cache_directory();
std::filesystem::path library_cache_file();
void load_input_settings();
void save_input_settings();
void load_graphics_settings();
void save_graphics_settings();
LibrarySettings load_library_settings();
void save_library_settings(const LibrarySettings &settings);
ConsoleSettings load_console_settings();
void save_console_settings(const ConsoleSettings &settings);
} // namespace xv
