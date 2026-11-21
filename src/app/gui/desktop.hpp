#pragma once
#include "launcher/config.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>

namespace xv {
struct LaunchResult;
struct DesktopHooks {
    // Decides whether a file can start. Called on a worker thread; never runs guest code.
    std::function<LaunchResult(const std::filesystem::path &)> load;
    // Runs a title that `load` reported as runnable, in a process and a window of its own,
    // and returns its exit code once it has ended. What that process prints goes to
    // `output` as it comes, in pieces that need not end where a line does. Called on a
    // separate thread, one title at a time, and `output` on that thread.
    std::function<std::uint32_t(const std::filesystem::path &,
                                const std::function<void(std::string_view)> &output)>
        run;
    // Ends the running title, after which `run` returns. Optional; File > Close App
    // needs it while a title runs.
    std::function<void()> close_running;
    // The input settings were changed in Options > Input; this is where they get saved,
    // for the next title started. May throw.
    std::function<void()> input_changed;
    std::function<void()> graphics_changed;
    LibrarySettings libraries;
    std::function<void(const LibrarySettings &)> libraries_changed;
    ConsoleSettings console;
    std::function<void(const ConsoleSettings &)> console_changed;
};
// Opens the main window, loading `initial` unless it is empty, and returns once it is closed.
// The result is the process exit code: that of the last title run, 1 if the last file opened
// could not start, and 0 if nothing was opened.
int run_desktop(const DesktopHooks &hooks, const std::filesystem::path &initial);
} // namespace xv
