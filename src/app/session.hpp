#pragma once
#include "app/version.hpp"
#include "host/session_log.hpp"
#include "launcher/config.hpp"
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

// What both executables do the same way.
namespace xv::app {
inline std::string text(const std::filesystem::path &path) {
    const auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}
// The helper of the session log is the executable itself, started again: its exit code if
// that is what this run is.
inline std::optional<int> session_log_helper(int argc, wchar_t **argv) {
    if (argc < 2 || std::filesystem::path(argv[1]) != host::helper_option)
        return std::nullopt;
    std::vector<std::wstring> arguments;
    for (int index = 2; index < argc; ++index)
        arguments.push_back(std::filesystem::path(argv[index]).wstring());
    return host::session_log_helper(arguments);
}
// Sessions in the window, and runs of a title, leave a log in the configuration folder.
// `who` starts the lines this executable prints.
inline void start_session_log(const char *who) {
    try {
        const auto log = host::start_session_log(config_directory() / "logs");
        std::cerr << who << " XValkyrie " << version << ", log " << text(log) << std::endl;
    } catch (const std::exception &e) {
        std::cerr << who << " no session log: " << e.what() << std::endl;
    }
}
} // namespace xv::app
