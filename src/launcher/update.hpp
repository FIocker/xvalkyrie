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
// The latest release on GitHub if it is later than `current`. Blocks on the network.
std::optional<Update> find_update(std::string_view current);
// Replaces the executables in this one's folder, elevating if the folder needs it. Throws
// std::runtime_error with the old ones back in place.
void install_update(const Update &update);
// `--apply-update FOLDER`, run by the elevated copy install_update starts
void apply_update(const std::filesystem::path &staged);
void restart();
void clear_replaced();
} // namespace xv
