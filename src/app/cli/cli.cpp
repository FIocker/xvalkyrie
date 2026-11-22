#include "app/cli/cli.hpp"
#include "analysis/assessment.hpp"
#include "formats/appx/manifest.hpp"
#include "formats/gdk/game_config.hpp"
#include "formats/pe/inspect.hpp"
#include "formats/xvd/identify.hpp"
#include "host/files.hpp"
#include "launcher/launch.hpp"
#include "loader/image.hpp"
#include "runtime/console.hpp"
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace xv::cli {
namespace {
std::vector<std::uint8_t> read_title(const std::filesystem::path &path) {
    return host::read_snapshot(title_executable(path), 256 * 1024 * 1024);
}
nlohmann::json inspect_file(const std::filesystem::path &path) {
    return pe::inspect(read_title(path));
}
nlohmann::json config_report(const std::filesystem::path &path) {
    return gdk::inspect_config(host::read_snapshot(path, gdk::config_limit));
}
} // namespace

const std::string_view commands =
    "  xvboot inspect FILE | inspect-config MicrosoftGame.config\n"
    "  xvboot inspect-manifest AppxManifest.xml\n"
    "  xvboot assess FILE [--config MicrosoftGame.config --entry NAME]\n"
    "  xvboot map FILE [--base HEX]     lay out the image; never runs it\n"
    "  xvboot inspect-package FILE      identify an XVD/XVC header; never opens it\n"
    "  xvboot bind FILE                 show where each import would bind; never runs it\n"
    "  xvboot properties FILE           all of that in one report, as the window shows it\n"
    "  xvboot modules durango|scarlett  the console modules and functions implemented here\n";

std::optional<int> run(int argc, wchar_t **argv) {
    if (argc < 2)
        return std::nullopt;
    const std::filesystem::path command(argv[1]);
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
        auto report = analysis::assess(pe);
        if (argc == 7) {
            const auto entry = std::filesystem::path(argv[6]).u8string();
            report["config_evidence"] = analysis::compare_declaration(
                pe, config_report(argv[4]), std::string(entry.begin(), entry.end()));
        }
        std::cout << report.dump(2) << '\n';
        return 0;
    }
    if (argc == 3 && command == "inspect-manifest") {
        const auto bytes = host::read_snapshot(argv[2], appx::manifest_limit);
        std::cout << appx::inspect_manifest(bytes).dump(2) << '\n';
        return 0;
    }
    if (argc == 3 && command == "inspect-package") {
        const auto prefix = host::read_prefix(argv[2], xvd::header_size);
        std::cout << xvd::identify(prefix.bytes, prefix.file_size).dump(2) << '\n';
        return 0;
    }
    if (command == "map" &&
        (argc == 3 || (argc == 5 && std::filesystem::path(argv[3]) == "--base"))) {
        loader::MapOptions options;
        if (argc == 5) {
            const auto text = std::filesystem::path(argv[4]).string();
            std::size_t used = 0;
            options.guest_base = std::stoull(text, &used, 16);
            if (used != text.size())
                throw std::invalid_argument("--base expects a hexadecimal guest address");
        }
        const auto bytes = read_title(argv[2]);
        std::cout << loader::MappedImage(bytes, options).report().dump(2) << '\n';
        return 0;
    }
    if (argc == 3 && command == "properties") {
        std::cout << title_properties(argv[2]).dump(2, ' ', false,
                                                    nlohmann::json::error_handler_t::replace)
                  << '\n';
        return 0;
    }
    if (argc == 3 && command == "bind") {
        const auto bytes = read_title(argv[2]);
        std::cout << runtime::bind_plan(
                         bytes, runtime::title_registry(bytes),
                         std::filesystem::absolute(title_executable(argv[2])).parent_path(),
                         runtime::console_system_directory(title_executable(argv[2])))
                         .dump(2)
                  << '\n';
        return 0;
    }
    if (argc == 3 && command == "modules") {
        const std::filesystem::path console(argv[2]);
        if (console != "durango" && console != "scarlett")
            throw std::invalid_argument("modules expects durango or scarlett");
        const auto target = console == "durango" ? Target::durango : Target::scarlett;
        std::cout << runtime::console_registry(target).describe().dump(2) << '\n';
        return 0;
    }
    return std::nullopt;
}
} // namespace xv::cli
