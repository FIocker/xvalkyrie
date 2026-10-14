#pragma once
#include <filesystem>

namespace xs {
// settings.json in the per-user configuration folder of the host
std::filesystem::path config_file();
// Applies the saved input settings; defaults if there are none or they cannot be read.
void load_input_settings();
// Writes the current input settings. Throws if the file cannot be written.
void save_input_settings();
} // namespace xs
