#pragma once
#include "core/target.hpp"
#include <filesystem>
#include <optional>
#include <string>

namespace xs {
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
LaunchResult launch_title(const std::filesystem::path &path);
} // namespace xs
