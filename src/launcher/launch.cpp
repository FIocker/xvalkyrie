#include "launcher/launch.hpp"
#include "analysis/modules.hpp"
#include "formats/appx/manifest.hpp"
#include "formats/gdk/game_config.hpp"
#include "formats/pe/inspect.hpp"
#include "formats/xvd/identify.hpp"
#include "host/files.hpp"
#include "launcher/config.hpp"
#include "loader/image.hpp"
#include "runtime/console.hpp"
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace xv {
namespace {
std::string hex(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    do
        text.insert(text.begin(), digits[value & 15]);
    while (value >>= 4);
    return "0x" + text;
}
std::string names(const nlohmann::json &profile, std::string_view prefix) {
    std::string text;
    for (const auto &module : profile.at("modules")) {
        if (module.at("category").get_ref<const std::string &>().starts_with(prefix))
            text += (text.empty() ? "" : ", ") + module.at("module").get<std::string>();
    }
    return text;
}
constexpr char manifest_name[] = "AppxManifest.xml", config_name[] = "MicrosoftGame.config";
bool named(const std::filesystem::path &path, std::string_view name) {
    const auto text = path.filename().string();
    return std::equal(text.begin(), text.end(), name.begin(), name.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}
// An ERA title has an AppxManifest.xml. A GDK title has a MicrosoftGame.config, and often
// an AppxManifest.xml made from it as well: the config says more, so it wins.
std::filesystem::path find_manifest(const std::filesystem::path &path) {
    if (!std::filesystem::is_directory(path))
        return named(path, manifest_name) || named(path, config_name) ? path
                                                                       : std::filesystem::path{};
    for (const auto &folder : {path, path / "Mount"})
        for (const auto *name : {config_name, manifest_name})
            if (std::filesystem::is_regular_file(folder / name))
                return folder / name;
    throw std::runtime_error("No MicrosoftGame.config or AppxManifest.xml in this directory or "
                             "its Mount subdirectory");
}
// a MicrosoftGame.config as the fields of a package manifest
nlohmann::json config_manifest(const nlohmann::json &config) {
    // the Series executable of a package that has one for each console
    const auto &executables = config.at("executables");
    const auto *chosen = &executables.at(0);
    for (const auto &executable : executables)
        if (executable.at("declared_device_family") == "Scarlett") {
            chosen = &executable;
            break;
        }
    return {{"name", config.at("name")},
            {"display_name", config.at("display_name")},
            {"version", config.at("version")},
            {"logo",
             config.at("large_logo").is_string() ? config.at("large_logo") : config.at("logo")},
            {"splash", config.at("splash_screen")},
            {"applications", {{{"executable", chosen->at("name")}}}}};
}
} // namespace

nlohmann::json title_manifest(const std::filesystem::path &path,
                              std::filesystem::path &directory) {
    const auto manifest = find_manifest(path);
    if (manifest.empty())
        return nullptr;
    directory = manifest.parent_path();
    if (named(manifest, config_name))
        return config_manifest(
            gdk::inspect_config(host::read_snapshot(manifest, gdk::config_limit)));
    return appx::inspect_manifest(host::read_snapshot(manifest, appx::manifest_limit));
}

namespace {
nlohmann::json read_manifest(const std::filesystem::path &path, std::filesystem::path &directory) {
    return title_manifest(path, directory);
}
std::filesystem::path executable_of(const nlohmann::json &manifest,
                                    const std::filesystem::path &directory) {
    const auto &name =
        manifest.at("applications")[0].at("executable").get_ref<const std::string &>();
    // a path inside the package, such as Binaries\\XboxOne\\Game.exe
    auto text = name;
    std::replace(text.begin(), text.end(), '\\', '/');
    const std::filesystem::path relative(std::u8string(text.begin(), text.end()));
    bool inside = !text.empty() && !relative.is_absolute() && text.find(':') == std::string::npos &&
                  text.front() != '/';
    for (const auto &part : relative)
        inside = inside && part != ".." && part != ".";
    if (!inside)
        throw std::runtime_error("Package manifest executable is not a path inside the package");
    return directory / relative;
}
} // namespace

std::filesystem::path title_executable(const std::filesystem::path &path) {
    std::filesystem::path directory;
    const auto manifest = read_manifest(path, directory);
    return manifest.is_null() ? path : executable_of(manifest, directory);
}

std::filesystem::path title_save_directory(const std::filesystem::path &path) {
    std::filesystem::path directory;
    auto manifest = read_manifest(path, directory);
    // an executable is the same title as the package it is in
    if (manifest.is_null()) {
        const auto package = runtime::package_directory(path.parent_path());
        if (std::filesystem::is_regular_file(package / manifest_name) ||
            std::filesystem::is_regular_file(package / config_name))
            manifest = read_manifest(package, directory);
    }
    const auto stem = path.stem().u8string();
    std::string name = manifest.is_object() && manifest.at("name").is_string()
                           ? manifest.at("name").get<std::string>()
                           : std::string(stem.begin(), stem.end());
    for (auto &c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_')
            c = '_';
    if (name.empty() || name.find_first_not_of('.') == std::string::npos)
        name = "title";
    return config_directory() / "saves" / name;
}

namespace {
LaunchResult launch_file(const std::filesystem::path &path) {
    const auto utf8 = path.u8string();
    LaunchResult result{{utf8.begin(), utf8.end()}, LaunchStatus::unreadable, {}};
    try {
        const auto prefix = host::read_prefix(path, xvd::header_size);
        if (xvd::has_magic(prefix.bytes)) {
            result.status = LaunchStatus::package_unsupported;
            const auto package = xvd::identify(prefix.bytes, prefix.file_size);
            result.detail = "This is an XVD package (content type " +
                            package.at("content_type_name").get<std::string>() + "; " +
                            package.at("encryption").get<std::string>() +
                            "). Packages cannot be verified, decrypted or mounted. Pass the "
                            "title's unpackaged executable instead.";
            return result;
        }
        const auto bytes = host::read_snapshot(path, 256 * 1024 * 1024);
        result.status = LaunchStatus::invalid_image;
        if (bytes.size() < 2 || bytes[0] != 'M' || bytes[1] != 'Z') {
            result.detail =
                "Expected an unpackaged x64 PE32+ executable. Package loading is not supported.";
            return result;
        }
        const auto report = pe::inspect(bytes, pe::Detail::light);
        if (report.at("machine") != 0x8664) {
            result.status = LaunchStatus::wrong_machine;
            result.detail = "This executable is not x86-64, the CPU architecture of Xbox One and "
                            "Xbox Series X|S.";
            return result;
        }
        const auto profile = analysis::import_profile(report);
        const auto &indication = profile.at("target_indication").get_ref<const std::string &>();
        if (indication == "pc" || indication == "mixed") {
            result.status = LaunchStatus::other_target;
            result.detail = indication == "pc"
                                ? "Imports " + names(profile, "d3d_") +
                                      ", the Direct3D runtime of a Windows PC build."
                                : std::string("Imports modules of more than one target.");
            result.detail +=
                " XValkyrie runs Xbox One and Xbox Series X|S builds. No guest code ran.";
            return result;
        }
        result.target = analysis::console_target(profile);
        if (!result.target) {
            result.status = LaunchStatus::runtime_unavailable;
            result.detail = "Read an x64 PE32+ executable. It imports no Xbox One or Xbox Series "
                            "X|S module, so its console target is unknown. No guest code ran.";
            return result;
        }
        const std::string console(target_name(*result.target));
        auto modules =
            names(profile, *result.target == Target::durango ? "d3d_xbox_one" : "d3d_scarlett");
        if (modules.empty())
            modules = names(profile, "legacy_era");
        // no relocations = can't rebase, map it at the preferred base just to look at it
        loader::MapOptions options;
        if (!report.at("directory_contents").contains("base_relocations"))
            options.guest_base = report.at("image_base").get<std::uint64_t>();
        const loader::MappedImage image(bytes, report, options);
        const auto &map = image.report();
        const auto plan = runtime::bind_plan(bytes, runtime::console_registry(*result.target),
                                             std::filesystem::absolute(path).parent_path(),
                                             runtime::console_system_directory(path));
        const auto missing = plan.at("missing").get<std::size_t>();
        result.detail =
            "Imports " + modules + ", which belong to an " + console + " build. Mapped " +
            hex(map.at("size_of_image")) + " bytes at " + hex(map.at("guest_base")) + " with " +
            std::to_string(map.at("relocation").at("dir64_fixups").get<std::uint64_t>()) +
            " relocations. Of " + std::to_string(plan.at("imports").size()) + " imports in " +
            std::to_string(profile.at("modules").size()) + " modules, " +
            std::to_string(plan.at("own").get<std::size_t>()) + " bind to XValkyrie, " +
            std::to_string(plan.at("host").get<std::size_t>()) + " to host APIs, " +
            std::to_string(plan.at("title").get<std::size_t>()) + " to the title's own DLLs, " +
            std::to_string(plan.at("system").get<std::size_t>()) +
            " to the console system modules it came with and " +
            std::to_string(missing) + " have no implementation";
        if (!missing && plan.at("imports").size()) {
            result.status = LaunchStatus::runnable;
            result.detail += ".";
            return result;
        }
        result.status = LaunchStatus::runtime_unavailable;
        std::size_t listed = 0;
        for (const auto &import : plan.at("imports")) {
            if (import.at("origin") != "missing" || listed == 4)
                continue;
            result.detail += (listed++ ? ", " : " (") + import.at("module").get<std::string>() +
                             "!" + import.at("symbol").get<std::string>();
        }
        result.detail += std::string(listed ? (missing > listed ? ", ...)" : ")") : "") +
                         ", so the entry point was not called. The " + console +
                         " runtime does not have them yet. No guest code ran.";
    } catch (const std::exception &e) {
        result.detail = e.what();
    }
    return result;
}
} // namespace

LaunchResult launch_title(const std::filesystem::path &path) {
    const auto utf8 = path.u8string();
    LaunchResult result{{utf8.begin(), utf8.end()}, LaunchStatus::unreadable, {}};
    try {
        std::filesystem::path directory;
        const auto manifest = read_manifest(path, directory);
        if (manifest.is_null())
            return launch_file(path);
        const auto label = [](const nlohmann::json &value) {
            return value.is_string() ? value.get<std::string>() : std::string("?");
        };
        const auto &application = manifest.at("applications")[0];
        const auto file = std::move(result.file);
        result = launch_file(executable_of(manifest, directory));
        result.file = file;
        result.detail = label(manifest.at("display_name")) + " " + label(manifest.at("version")) +
                        ", executable " + label(application.at("executable")) + ". " +
                        result.detail;
    } catch (const std::exception &e) {
        result.detail = e.what();
    }
    return result;
}
} // namespace xv
