#include "app/launch.hpp"
#include "app/version.hpp"
#include "compatibility/assessment.hpp"
#include "formats/gdk/game_config.hpp"
#include "formats/pe/inspect.hpp"
#include "formats/xvd/identify.hpp"
#include "frontend/desktop.hpp"
#include "loader/image.hpp"
#include "platform/host/files.hpp"
#include "runtime/console.hpp"
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>

namespace {
nlohmann::json inspect_file(const std::filesystem::path &path) {
    const auto bytes = xs::host::read_snapshot(path, 256 * 1024 * 1024);
    return xs::pe::inspect(bytes);
}
nlohmann::json config_report(const std::filesystem::path &path) {
    return xs::gdk::inspect_config(xs::host::read_snapshot(path, xs::gdk::config_limit));
}
constexpr std::string_view usage =
    "Usage: xscarlet [FILE]\n"
    "Targets: Xbox One (Durango), Xbox Series X|S (Scarlett)\n\n"
    "Opens the XScarlet window, loading FILE if one is given.\n\n"
    "Optional verification:\n"
    "  xscarlet inspect FILE | inspect-config MicrosoftGame.config\n"
    "  xscarlet assess FILE [--config MicrosoftGame.config --entry NAME]\n"
    "  xscarlet map FILE [--base HEX]     lay out the image; never runs it\n"
    "  xscarlet inspect-package FILE      identify an XVD/XVC header; never opens it\n"
    "  xscarlet bind FILE                 show where each import would bind; never runs it\n\n"
    "Developer (Windows host), RUNS THE FILE'S CODE regardless of its target:\n"
    "  xscarlet exec FILE [--allow-missing]\n";
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
    try {
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
            const auto bytes = xs::host::read_snapshot(argv[2], 256 * 1024 * 1024);
            std::cout << xs::loader::MappedImage(bytes, options).report().dump(2) << '\n';
            return 0;
        }
        if (argc == 3 && command == "bind") {
            const auto bytes = xs::host::read_snapshot(argv[2], 256 * 1024 * 1024);
            std::cout << xs::runtime::bind_plan(bytes, xs::runtime::title_registry(bytes)).dump(2)
                      << '\n';
            return 0;
        }
        if (command == "exec" &&
            (argc == 3 || (argc == 4 && std::filesystem::path(argv[3]) == "--allow-missing"))) {
            const auto bytes = xs::host::read_snapshot(argv[2], 256 * 1024 * 1024);
            return static_cast<int>(xs::runtime::execute(
                argv[2], bytes, xs::runtime::title_registry(bytes), argc == 4));
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
            auto title = xs::launch_title(path);
            if (title.status != xs::LaunchStatus::runnable)
                std::cerr << "XScarlet: " << title.file << ": " << title.detail << std::endl;
            return title;
        };
        hooks.run = [](const std::filesystem::path &path) {
            const auto bytes = xs::host::read_snapshot(path, 256 * 1024 * 1024);
            return xs::runtime::execute(path, bytes, xs::runtime::title_registry(bytes), false);
        };
        // A title that could not start exits 1 even though its window was closed normally.
        return xs::run_desktop(hooks, command);
    } catch (const std::exception &e) {
        std::cerr << "xscarlet: " << e.what() << '\n';
        return 1;
    }
}
