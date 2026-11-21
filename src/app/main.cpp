#include "analysis/assessment.hpp"
#include "app/ui/desktop.hpp"
#include "app/version.hpp"
#include "backends/graphics/backend.hpp"
#include "backends/input/input.hpp"
#include "backends/surface/surface.hpp"
#include "formats/appx/manifest.hpp"
#include "formats/gdk/game_config.hpp"
#include "formats/pe/inspect.hpp"
#include "formats/xvd/identify.hpp"
#include "host/files.hpp"
#include "host/session_log.hpp"
#include "launcher/config.hpp"
#include "launcher/launch.hpp"
#include "loader/image.hpp"
#include "runtime/console.hpp"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::vector<std::uint8_t> read_title(const std::filesystem::path &path) {
    return xv::host::read_snapshot(xv::title_executable(path), 256 * 1024 * 1024);
}
nlohmann::json inspect_file(const std::filesystem::path &path) {
    return xv::pe::inspect(read_title(path));
}
nlohmann::json config_report(const std::filesystem::path &path) {
    return xv::gdk::inspect_config(xv::host::read_snapshot(path, xv::gdk::config_limit));
}
std::string text(const std::filesystem::path &path) {
    const auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}
std::wstring wide(const std::string &utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())).wstring();
}
// what config.ini says, the way the runtime takes it
xv::runtime::ConsoleSettings console_settings() {
    const auto saved = xv::load_console_settings();
    return {wide(saved.gamertag), wide(saved.xbox_user_id), saved.gamerscore, saved.age_group,
            saved.headset,        saved.keyboard_dialog};
}
// How the window starts a title: this executable again, with the surface to draw into
constexpr std::string_view title_process_option = "--title-process";
std::uint32_t run_title(const std::filesystem::path &path) {
    const auto file = xv::title_executable(path);
    const auto bytes = xv::host::read_snapshot(file, 256 * 1024 * 1024);
    xv::runtime::ExecuteOptions options;
    options.save_directory = xv::title_save_directory(path);
    options.shader_cache_directory = xv::cache_directory() / "shaders";
    options.console = console_settings();
    std::cerr << "[Launcher] saves in " << text(options.save_directory) << std::endl;
    return xv::runtime::execute(file, bytes, xv::runtime::title_registry(bytes), options);
}
constexpr std::string_view usage =
    "Usage: xvalkyrie [TITLE]\n"
    "Targets: Xbox One (Durango), Xbox Series X|S (Scarlett)\n\n"
    "Opens the XValkyrie window, loading TITLE if one is given. TITLE and every FILE below\n"
    "are an executable, or an unpacked title: its directory or its AppxManifest.xml.\n\n"
    "Optional verification:\n"
    "  xvalkyrie inspect FILE | inspect-config MicrosoftGame.config\n"
    "  xvalkyrie inspect-manifest AppxManifest.xml\n"
    "  xvalkyrie assess FILE [--config MicrosoftGame.config --entry NAME]\n"
    "  xvalkyrie map FILE [--base HEX]     lay out the image; never runs it\n"
    "  xvalkyrie inspect-package FILE      identify an XVD/XVC header; never opens it\n"
    "  xvalkyrie bind FILE                 show where each import would bind; never runs it\n\n"
    "Developer (Windows host), RUNS THE FILE'S CODE regardless of its target:\n"
    "  xvalkyrie exec FILE [--allow-missing] [--trace] [--mute] [--backend d3d11|vulkan]\n"
    "Backends: d3d11 uses the host's Direct3D 11; vulkan translates Direct3D 11 to Vulkan\n"
    "(needs xv_d3d11.dll and xv_dxgi.dll next to xvalkyrie.exe).\n";
} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        if (argc > 1 && std::filesystem::path(argv[1]) == xv::host::helper_option) {
            std::vector<std::wstring> arguments;
            for (int index = 2; index < argc; ++index)
                arguments.push_back(std::filesystem::path(argv[index]).wstring());
            return xv::host::session_log_helper(arguments);
        }
        // Sessions in the window, and runs of a title, leave a log next to the executable.
        const std::filesystem::path first =
            argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
        if (argc == 1 || first == "exec" || first == title_process_option ||
            (argc == 2 && first.native()[0] != '-')) {
            try {
                const auto log = xv::host::start_session_log(xv::config_directory() / "logs");
                std::cerr << "[Launcher] XValkyrie " << xv::version << ", log " << text(log)
                          << std::endl;
            } catch (const std::exception &e) {
                std::cerr << "[Launcher] no session log: " << e.what() << std::endl;
            }
        }
        xv::load_input_settings();
        xv::load_graphics_settings();
        const std::filesystem::path command =
            argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
        if (argc == 4 && command == title_process_option) {
            auto *const surface = reinterpret_cast<void *>(
                static_cast<std::uintptr_t>(std::stoull(std::filesystem::path(argv[2]).string())));
            xv::surface::use_title_surface(surface);
            // the keys the launcher's window sees do not come here: read from the system,
            // while that window is the one in front
            auto input = xv::input::settings();
            input.capture = xv::input::Capture::direct;
            xv::input::configure(input);
            xv::input::set_foreground_window(xv::surface::title_surface_owner());
            return static_cast<int>(run_title(argv[3]));
        }
        if (argc == 3 && command == "inspect") {
            std::cout << inspect_file(argv[2]).dump(2) << '\n';
            return 0;
        }
        if (argc == 3 && command == "inspect-config") {
            std::cout << config_report(argv[2]).dump(2) << '\n';
            return 0;
        }
        if (command == "assess" &&
            (argc == 3 || (argc == 7 && std::filesystem::path(argv[3]) == "--config" &&
                           std::filesystem::path(argv[5]) == "--entry"))) {
            const auto pe = inspect_file(argv[2]);
            auto report = xv::analysis::assess(pe);
            if (argc == 7) {
                const auto entry = std::filesystem::path(argv[6]).u8string();
                report["config_evidence"] = xv::analysis::compare_declaration(
                    pe, config_report(argv[4]), std::string(entry.begin(), entry.end()));
            }
            std::cout << report.dump(2) << '\n';
            return 0;
        }
        if (argc == 3 && command == "inspect-manifest") {
            const auto bytes = xv::host::read_snapshot(argv[2], xv::appx::manifest_limit);
            std::cout << xv::appx::inspect_manifest(bytes).dump(2) << '\n';
            return 0;
        }
        if (argc == 3 && command == "inspect-package") {
            const auto prefix = xv::host::read_prefix(argv[2], xv::xvd::header_size);
            std::cout << xv::xvd::identify(prefix.bytes, prefix.file_size).dump(2) << '\n';
            return 0;
        }
        if (command == "map" &&
            (argc == 3 || (argc == 5 && std::filesystem::path(argv[3]) == "--base"))) {
            xv::loader::MapOptions options;
            if (argc == 5) {
                const auto text = std::filesystem::path(argv[4]).string();
                std::size_t used = 0;
                options.guest_base = std::stoull(text, &used, 16);
                if (used != text.size())
                    throw std::invalid_argument("--base expects a hexadecimal guest address");
            }
            const auto bytes = read_title(argv[2]);
            std::cout << xv::loader::MappedImage(bytes, options).report().dump(2) << '\n';
            return 0;
        }
        if (argc == 3 && command == "bind") {
            const auto bytes = read_title(argv[2]);
            std::cout << xv::runtime::bind_plan(
                             bytes, xv::runtime::title_registry(bytes),
                             std::filesystem::absolute(xv::title_executable(argv[2])).parent_path(),
                             xv::runtime::console_system_directory(xv::title_executable(argv[2])))
                             .dump(2)
                      << '\n';
            return 0;
        }
        if (command == "exec" && argc >= 3) {
            xv::runtime::ExecuteOptions options;
            for (int i = 3; i < argc; ++i) {
                const std::filesystem::path flag(argv[i]);
                if (flag == "--allow-missing")
                    options.allow_missing = true;
                else if (flag == "--trace")
                    options.trace = true;
                else if (flag == "--mute")
                    options.mute = true;
                else if (flag == "--backend" && i + 1 < argc) {
                    const auto backend =
                        xv::graphics::parse_backend(std::filesystem::path(argv[++i]).string());
                    if (!backend)
                        throw std::invalid_argument("exec: --backend expects d3d11 or vulkan");
                    xv::graphics::select_backend(*backend);
                } else
                    throw std::invalid_argument("exec: unknown option");
            }
            const auto bytes = read_title(argv[2]);
            options.save_directory = xv::title_save_directory(argv[2]);
            options.shader_cache_directory = xv::cache_directory() / "shaders";
            options.console = console_settings();
            return static_cast<int>(xv::runtime::execute(
                xv::title_executable(argv[2]), bytes, xv::runtime::title_registry(bytes), options));
        }
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
        // A title runs in a process of its own, drawing into the surface of this window:
        // it can then be closed, and another opened, without this window going away.
        static std::atomic<void *> title_process{nullptr};
        hooks.run = [](const std::filesystem::path &path) {
            std::cerr << "[Launcher] starting " << text(xv::title_executable(path)) << std::endl;
            const auto surface = reinterpret_cast<std::uintptr_t>(xv::surface::title_surface());
            const auto code = xv::host::run_self(
                {wide(std::string(title_process_option)), std::to_wstring(surface), path.wstring()},
                title_process);
            xv::surface::hide_title_surface();
            std::cerr << "[Launcher] the title ended with code 0x" << std::hex << code << std::dec
                      << std::endl;
            return code;
        };
        hooks.starting = xv::surface::renew_title_surface;
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
        hooks.host = xv::surface::set_title_parent;
        hooks.area = xv::surface::set_title_area;
        hooks.showing = xv::surface::title_window_shown;
        hooks.covered = xv::surface::set_title_covered;
        hooks.holes = [](const std::vector<std::array<int, 4>> &holes) {
            std::vector<xv::surface::Rectangle> cut;
            for (const auto &hole : holes)
                cut.push_back({hole[0], hole[1], hole[2], hole[3]});
            xv::surface::set_title_holes(cut);
        };
        hooks.frames = xv::graphics::frames_presented;
        // A title that could not start exits 1 even though its window was closed normally.
        return xv::run_desktop(hooks, command);
    } catch (const std::exception &e) {
        std::cerr << "[Launcher] " << e.what() << '\n';
        return 1;
    }
}
