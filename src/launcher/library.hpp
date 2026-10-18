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
// Every title under `roots`. With a `cache` file, a title whose manifest and executable did
// not change since the last scan is taken from it instead of being read again, and the
// result is written back to it.
std::vector<LibraryTitle> discover_library(const std::vector<std::filesystem::path> &roots,
                                           const std::filesystem::path &cache = {});
// The titles under `roots` that the last scan wrote to `cache` and that are still there,
// without reading any of them: what to show while discover_library runs. Never throws.
std::vector<LibraryTitle> cached_library(const std::vector<std::filesystem::path> &roots,
                                         const std::filesystem::path &cache);
} // namespace xs
