#include "app/gui/desktop.hpp"
#include "app/session.hpp"
#include "app/version.hpp"
#include "backends/graphics/backend.hpp"
#include "backends/input/input.hpp"
#include "host/files.hpp"
#include "launcher/config.hpp"
#include "launcher/launch.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

// xvalkyrie: the window. It never runs a title itself: each one runs in xvboot, the
// executable next to this one, in a process and a window of its own.
namespace {
using xv::app::text;
constexpr wchar_t boot[] = L"xvboot.exe";
constexpr std::string_view usage =
    "Usage: xvalkyrie [TITLE]\n"
    "Targets: Xbox One (Durango), Xbox Series X|S (Scarlett)\n\n"
    "Opens the XValkyrie window, loading TITLE if one is given: an executable, or an\n"
    "unpacked title (its directory or its AppxManifest.xml). Titles run in xvboot, which\n"
    "also has the commands that look at one: see xvboot --help.\n";
} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        if (const auto code = xv::app::session_log_helper(argc, argv))
            return *code;
        xv::host::use_parent_console();
        const std::filesystem::path command =
            argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
        if (argc == 2 && command == "--help") {
            std::cout << usage;
            return 0;
        }
        if (argc == 2 && command == "--version") {
            std::cout << "XValkyrie " << xv::version << '\n';
            return 0;
        }
        if (argc > 2 || (argc == 2 && (command.empty() || argv[1][0] == '-'))) {
            std::cerr << usage;
            return 2;
        }
        xv::app::start_session_log("[Launcher]");
        xv::load_input_settings();
        xv::load_graphics_settings();
        if (!std::filesystem::exists(xv::config_file())) {
            try {
                xv::save_input_settings();
            } catch (const std::exception &e) {
                std::cerr << "[Launcher] could not create config.ini: " << e.what() << '\n';
            }
        }
        xv::DesktopHooks hooks;
        hooks.libraries = xv::load_library_settings();
        hooks.libraries_changed = xv::save_library_settings;
        hooks.console = xv::load_console_settings();
        hooks.console_changed = [](const xv::ConsoleSettings &settings) {
            xv::save_console_settings(settings);
            std::cerr << "[Launcher] console: " << settings.gamertag << ", saved to "
                      << text(xv::config_file()) << std::endl;
        };
        hooks.load = [](const std::filesystem::path &path) {
            std::cerr << "[Launcher] opening " << text(path) << std::endl;
            auto title = xv::launch_title(path);
            std::cerr << "[Launcher] "
                      << (title.status == xv::LaunchStatus::runnable ? "can start: "
                                                                     : "cannot start: ")
                      << title.detail << std::endl;
            return title;
        };
        // A title runs in a process and a window of its own: it can then be closed, and
        // another opened, without this window going away, and neither can hold the other up.
        static std::atomic<void *> title_process{nullptr};
        hooks.run = [](const std::filesystem::path &path,
                       const std::function<void(std::string_view)> &output) {
            std::cerr << "[Launcher] starting " << text(xv::title_executable(path)) << std::endl;
            // what the title's process prints: on the window, and where this one prints
            const auto code = xv::host::run_beside(
                boot, {L"exec", path.wstring()}, title_process, [&](std::string_view piece) {
                    std::cerr.write(piece.data(), static_cast<std::streamsize>(piece.size()));
                    output(piece);
                });
            std::cerr << "[Launcher] the title ended with code 0x" << std::hex << code << std::dec
                      << std::endl;
            return code;
        };
        hooks.close_running = [] {
            std::cerr << "[Launcher] closing the title" << std::endl;
            xv::host::end_process(title_process.load());
        };
        hooks.input_changed = [] {
            xv::save_input_settings();
            const auto settings = xv::input::settings();
            std::cerr << "[Launcher] input: "
                      << (settings.capture == xv::input::Capture::direct ? "direct" : "window")
                      << " capture, saved to " << text(xv::config_file()) << std::endl;
        };
        hooks.graphics_changed = [] {
            xv::save_graphics_settings();
            std::cerr << "[Launcher] graphics: "
                      << xv::graphics::backend_name(xv::graphics::selected_backend())
                      << " backend, saved to " << text(xv::config_file()) << std::endl;
        };
        // A title that could not start exits 1 even though its window was closed normally.
        return xv::run_desktop(hooks, command);
    } catch (const std::exception &e) {
        std::cerr << "[Launcher] " << e.what() << '\n';
        return 1;
    }
}
