#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace xv {
struct Update {
    std::string version; // the release's tag
    std::string archive; // URL of its portable archive
};
struct UpdateCheck {
    std::optional<Update> update; // a release later than this build
    std::string message;          // what to tell the user when there is none
};
// Compares `current` with the latest release on GitHub. Blocks on the network; never throws.
UpdateCheck find_update(std::string_view current);
// Replaces the executables in this one's folder, elevating if the folder needs it. Throws
// std::runtime_error with the old ones back in place.
void install_update(const Update &update);
// `--apply-update FOLDER`, run by the elevated copy install_update starts
void apply_update(const std::filesystem::path &staged);
void restart();
void clear_replaced();
} // namespace xv
