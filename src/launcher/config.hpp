#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xs {
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
};
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
} // namespace xs
