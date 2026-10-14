#include "app/config.hpp"
#include "app/launch.hpp"
#include "app/version.hpp"
#include "compatibility/assessment.hpp"
#include "formats/appx/manifest.hpp"
#include "formats/gdk/game_config.hpp"
#include "formats/pe/inspect.hpp"
#include "formats/xvd/identify.hpp"
#include "frontend/desktop.hpp"
#include "graphics/backend.hpp"
#include "input/input.hpp"
#include "loader/image.hpp"
#include "platform/host/files.hpp"
#include "platform/host/session_log.hpp"
#include "runtime/common/window.hpp"
#include "runtime/console.hpp"
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::vector<std::uint8_t> read_title(const std::filesystem::path &path) {
    return xs::host::read_snapshot(xs::title_executable(path), 256 * 1024 * 1024);
}
nlohmann::json inspect_file(const std::filesystem::path &path) {
    return xs::pe::inspect(read_title(path));
}
nlohmann::json config_report(const std::filesystem::path &path) {
    return xs::gdk::inspect_config(xs::host::read_snapshot(path, xs::gdk::config_limit));
}
std::string text(const std::filesystem::path &path) {
    const auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}
constexpr std::string_view usage =
    "Usage: xscarlet [TITLE]\n"
    "Targets: Xbox One (Durango), Xbox Series X|S (Scarlett)\n\n"
    "Opens the XScarlet window, loading TITLE if one is given. TITLE and every FILE below\n"
    "are an executable, or an unpacked title: its directory or its AppxManifest.xml.\n\n"
    "Optional verification:\n"
    "  xscarlet inspect FILE | inspect-config MicrosoftGame.config\n"
    "  xscarlet inspect-manifest AppxManifest.xml\n"
    "  xscarlet assess FILE [--config MicrosoftGame.config --entry NAME]\n"
    "  xscarlet map FILE [--base HEX]     lay out the image; never runs it\n"
    "  xscarlet inspect-package FILE      identify an XVD/XVC header; never opens it\n"
    "  xscarlet bind FILE                 show where each import would bind; never runs it\n\n"
    "Developer (Windows host), RUNS THE FILE'S CODE regardless of its target:\n"
    "  xscarlet exec FILE [--allow-missing] [--trace] [--mute] [--backend d3d11|vulkan]\n"
    "Backends: d3d11 uses the host's Direct3D 11 (Windows only); vulkan cannot render titles "
    "yet.\n";
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
    try {
        if (argc > 1 && std::filesystem::path(argv[1]) == xs::host::helper_option) {
            std::vector<std::wstring> arguments;
            for (int index = 2; index < argc; ++index)
                arguments.push_back(std::filesystem::path(argv[index]).wstring());
            return xs::host::session_log_helper(arguments);
        }
        // Sessions in the window, and runs of a title, leave a log next to the executable.
        const std::filesystem::path first =
            argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
        if (argc == 1 || first == "exec" || (argc == 2 && first.native()[0] != '-')) {
            try {
                const auto log =
                    xs::host::start_session_log(xs::host::executable_directory() / "logs");
                std::cerr << "[Launcher] XScarlet " << xs::version << ", log " << text(log)
                          << std::endl;
            } catch (const std::exception &e) {
                std::cerr << "[Launcher] no session log: " << e.what() << std::endl;
            }
        }
        xs::load_input_settings();
        const std::filesystem::path command =
            argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
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
            auto report = xs::compat::assess(pe);
            if (argc == 7) {
                const auto entry = std::filesystem::path(argv[6]).u8string();
                report["config_evidence"] = xs::compat::compare_declaration(
                    pe, config_report(argv[4]), std::string(entry.begin(), entry.end()));
            }
            std::cout << report.dump(2) << '\n';
            return 0;
        }
        if (argc == 3 && command == "inspect-manifest") {
            const auto bytes = xs::host::read_snapshot(argv[2], xs::appx::manifest_limit);
            std::cout << xs::appx::inspect_manifest(bytes).dump(2) << '\n';
            return 0;
        }
        if (argc == 3 && command == "inspect-package") {
            const auto prefix = xs::host::read_prefix(argv[2], xs::xvd::header_size);
            std::cout << xs::xvd::identify(prefix.bytes, prefix.file_size).dump(2) << '\n';
            return 0;
        }
        if (command == "map" &&
            (argc == 3 || (argc == 5 && std::filesystem::path(argv[3]) == "--base"))) {
            xs::loader::MapOptions options;
            if (argc == 5) {
                const auto text = std::filesystem::path(argv[4]).string();
                std::size_t used = 0;
                options.guest_base = std::stoull(text, &used, 16);
                if (used != text.size())
                    throw std::invalid_argument("--base expects a hexadecimal guest address");
            }
            const auto bytes = read_title(argv[2]);
            std::cout << xs::loader::MappedImage(bytes, options).report().dump(2) << '\n';
            return 0;
        }
        if (argc == 3 && command == "bind") {
            const auto bytes = read_title(argv[2]);
            std::cout << xs::runtime::bind_plan(
                             bytes, xs::runtime::title_registry(bytes),
                             std::filesystem::absolute(xs::title_executable(argv[2])).parent_path())
                             .dump(2)
                      << '\n';
            return 0;
        }
        if (command == "exec" && argc >= 3) {
            xs::runtime::ExecuteOptions options;
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
                        xs::graphics::parse_backend(std::filesystem::path(argv[++i]).string());
                    if (!backend)
                        throw std::invalid_argument("exec: --backend expects d3d11 or vulkan");
                    xs::graphics::select_backend(*backend);
                } else
                    throw std::invalid_argument("exec: unknown option");
            }
            const auto bytes = read_title(argv[2]);
            return static_cast<int>(xs::runtime::execute(
                xs::title_executable(argv[2]), bytes, xs::runtime::title_registry(bytes), options));
        }
        if (argc == 2 && command == "--help") {
            std::cout << usage;
            return 0;
        }
        if (argc == 2 && command == "--version") {
            std::cout << "XScarlet " << xs::version << '\n';
            return 0;
        }
        if (argc > 2 || (argc == 2 && (command.empty() || argv[1][0] == '-'))) {
            std::cerr << usage;
            return 2;
        }
        xs::DesktopHooks hooks;
        hooks.load = [](const std::filesystem::path &path) {
            std::cerr << "[Launcher] opening " << text(path) << std::endl;
            auto title = xs::launch_title(path);
            std::cerr << "[Launcher] "
                      << (title.status == xs::LaunchStatus::runnable ? "can start: "
                                                                     : "cannot start: ")
                      << title.detail << std::endl;
            return title;
        };
        hooks.run = [](const std::filesystem::path &path) {
            const auto file = xs::title_executable(path);
            std::cerr << "[Launcher] starting " << text(file) << std::endl;
            const auto bytes = xs::host::read_snapshot(file, 256 * 1024 * 1024);
            const auto code = xs::runtime::execute(file, bytes, xs::runtime::title_registry(bytes));
            std::cerr << "[Launcher] the title ended with code 0x" << std::hex << code << std::dec
                      << std::endl;
            return code;
        };
        hooks.input_changed = [] {
            xs::save_input_settings();
            const auto settings = xs::input::settings();
            std::cerr << "[Launcher] input: "
                      << (settings.capture == xs::input::Capture::direct ? "direct" : "window")
                      << " capture, saved to " << text(xs::config_file()) << std::endl;
        };
        hooks.host = xs::runtime::set_title_parent;
        hooks.area = xs::runtime::set_title_area;
        hooks.showing = xs::runtime::title_window_shown;
        // A title that could not start exits 1 even though its window was closed normally.
        return xs::run_desktop(hooks, command);
    } catch (const std::exception &e) {
        std::cerr << "[Launcher] " << e.what() << '\n';
        return 1;
    }
}
