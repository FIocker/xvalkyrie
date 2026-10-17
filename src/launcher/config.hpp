#pragma once
#include <filesystem>
#include <vector>

namespace xs {
struct LibrarySettings {
    std::vector<std::filesystem::path> directories;
    bool grid = true;
};
// config.ini and logs/ live in the per-user configuration folder.
std::filesystem::path config_directory();
std::filesystem::path config_file();
void load_input_settings();
void save_input_settings();
LibrarySettings load_library_settings();
void save_library_settings(const LibrarySettings &settings);
} // namespace xs
