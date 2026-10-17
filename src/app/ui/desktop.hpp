#pragma once
#include "launcher/config.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>

namespace xs {
struct LaunchResult;
struct DesktopHooks {
    // Decides whether a file can start. Called on a worker thread; never runs guest code.
    std::function<LaunchResult(const std::filesystem::path &)> load;
    // Runs a title that `load` reported as runnable and returns its exit code. Called on a
    // separate thread, one title at a time.
    std::function<std::uint32_t(const std::filesystem::path &)> run;
    // The title draws inside the main window. `host` is called once with the native handle
    // of that window (null where there is none to give), `area` whenever the rectangle left
    // for the title changes, in pixels, and `showing` tells whether the title has put its
    // first frame there. All optional and called on the window thread.
    std::function<void(void *native_window)> host;
    std::function<void(int x, int y, int width, int height)> area;
    std::function<bool()> showing;
    // The input settings were changed in Options > Input and are already in effect; this
    // is where they get saved. May throw.
    std::function<void()> input_changed;
    LibrarySettings libraries;
    std::function<void(const LibrarySettings &)> libraries_changed;
};
// Opens the main window, loading `initial` unless it is empty, and returns once it is closed.
// The result is the process exit code: that of the last title run, 1 if the last file opened
// could not start, and 0 if nothing was opened.
int run_desktop(const DesktopHooks &hooks, const std::filesystem::path &initial);
} // namespace xs
