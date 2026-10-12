#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>

namespace xs {
struct LaunchResult;
struct DesktopHooks {
    // Decides whether a file can start. Called on the window thread; never runs guest code.
    std::function<LaunchResult(const std::filesystem::path &)> load;
    // Runs a title that `load` reported as runnable and returns its exit code. Called on a
    // separate thread, one title at a time.
    std::function<std::uint32_t(const std::filesystem::path &)> run;
};
// Opens the main window, loading `initial` unless it is empty, and returns once it is closed.
// The result is the process exit code: that of the last title run, 1 if the last file opened
// could not start, and 0 if nothing was opened.
int run_desktop(const DesktopHooks &hooks, const std::filesystem::path &initial);
} // namespace xs
