#pragma once
#include <filesystem>
#include <vector>

namespace xs {
struct LibrarySettings {
    std::vector<std::filesystem::path> directories;
    bool grid = true;
};
// config.ini, logs/, saves/ and cache/ live in the per-user configuration folder.
std::filesystem::path config_directory();
std::filesystem::path config_file();
// what can be worked out again but takes a while, such as the game library
std::filesystem::path cache_directory();
std::filesystem::path library_cache_file();
void load_input_settings();
void save_input_settings();
LibrarySettings load_library_settings();
void save_library_settings(const LibrarySettings &settings);
} // namespace xs
