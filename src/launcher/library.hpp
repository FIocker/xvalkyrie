#pragma once
#include "core/target.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace xs {
struct LibraryTitle {
    std::filesystem::path path;       // title directory or XVD/XVC file
    std::filesystem::path executable; // empty for packages
    std::filesystem::path logo;       // empty if unavailable or unsafe
    std::filesystem::path splash;     // picture shown while it starts; empty like logo
    std::string name;
    std::string identity;
    std::string version;
    std::optional<Target> target; // import-based estimate; never a verified console build
    bool package = false;
};
std::optional<LibraryTitle> discover_title(const std::filesystem::path &path);
// The splash picture of the title `path` stands for (its directory, its manifest or its
// executable), read from the manifest alone. Empty if there is none; never throws.
std::filesystem::path title_splash(const std::filesystem::path &path);
std::vector<LibraryTitle> discover_library(const std::vector<std::filesystem::path> &roots);
} // namespace xs
