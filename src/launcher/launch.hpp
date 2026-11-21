#pragma once
#include "core/target.hpp"
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>

namespace xv {
enum class LaunchStatus {
    unreadable,
    invalid_image,
    wrong_machine,
    package_unsupported, // XVD/XVC container; identified but never opened
    other_target,        // imports indicate a Windows PC build or a mix of targets
    runtime_unavailable,
    runnable // console target known, every import bound, host can execute; nothing ran yet
};
struct LaunchResult {
    std::string file;
    LaunchStatus status;
    std::string detail;
    std::optional<Target> target = {}; // console the imports point to, if any
};
// `path` is an executable, an unpacked title directory, or the MicrosoftGame.config or
// AppxManifest.xml of one.
LaunchResult launch_title(const std::filesystem::path &path);
// What the title `path` stands for says about itself (name, display_name, version, logo,
// splash, applications), from its MicrosoftGame.config or its AppxManifest.xml, looked up in
// the directory and in its Mount subdirectory. `directory` is set to where it was found.
// Null if `path` is a file that is neither.
nlohmann::json title_manifest(const std::filesystem::path &path, std::filesystem::path &directory);
// The executable `path` stands for: the first application of the manifest for a title
// directory (looked up in it and in its Mount subdirectory), `path` itself otherwise.
std::filesystem::path title_executable(const std::filesystem::path &path);
// Where the saved games of the title `path` stands for are kept: saves/<package name> in the
// configuration folder, or saves/<executable name> for a title without a manifest.
std::filesystem::path title_save_directory(const std::filesystem::path &path);
} // namespace xv
