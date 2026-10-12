#pragma once
#include <filesystem>
#include <string>

namespace xs {
enum class LaunchStatus {
    unreadable,
    invalid_image,
    wrong_machine,
    package_unsupported, // XVD/XVC container; identified but never opened
    other_target,        // imports indicate Windows PC, Xbox One or ERA rather than Scarlett
    runtime_unavailable,
    runnable // Scarlett-indicated, every import bound, host can execute; nothing ran yet
};
struct LaunchResult {
    std::string file;
    LaunchStatus status;
    std::string detail;
};
LaunchResult launch_title(const std::filesystem::path &path);
} // namespace xs
