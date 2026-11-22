#include "launcher/launch.hpp"
#include "analysis/modules.hpp"
#include "formats/appx/manifest.hpp"
#include "formats/gdk/game_config.hpp"
#include "formats/pe/inspect.hpp"
#include "formats/pri/resources.hpp"
#include "formats/xvd/identify.hpp"
#include "host/files.hpp"
#include "launcher/config.hpp"
#include "loader/image.hpp"
#include "runtime/console.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

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
    auto logos = nlohmann::json::array();
    for (const auto *key : {"large_logo", "logo", "small_logo"})
        if (config.at(key).is_string())
            logos.push_back(config.at(key));
    return {
        {"name", config.at("name")},
        {"display_name", config.at("display_name")},
        {"version", config.at("version")},
        {"publisher", config.at("publisher_display_name")},
        {"title_id", config.at("title_id")},
        {"store_id", config.at("store_id")},
        {"logos", std::move(logos)},
        {"store_logo", config.at("store_logo")},
        {"logo", config.at("large_logo").is_string() ? config.at("large_logo") : config.at("logo")},
        {"splash", config.at("splash_screen")},
        {"applications", {{{"executable", chosen->at("name")}}}}};
}
} // namespace

nlohmann::json title_manifest(const std::filesystem::path &path, std::filesystem::path &directory) {
    const auto manifest = find_manifest(path);
    if (manifest.empty())
        return nullptr;
    directory = manifest.parent_path();
    auto info =
        named(manifest, config_name)
            ? config_manifest(gdk::inspect_config(host::read_snapshot(manifest, gdk::config_limit)))
            : appx::inspect_manifest(host::read_snapshot(manifest, appx::manifest_limit));
    // A display name that is a reference into the package's resources ("ms-resource:AppName")
    // is replaced by what it stands for, from the resource index next to the manifest; the
    // reference is kept beside it. A package without a readable index keeps the reference.
    if (const auto &name = info.at("display_name");
        name.is_string() && name.get_ref<const std::string &>().starts_with("ms-resource:")) {
        try {
            const auto reference = name.get<std::string>();
            const auto index = directory / "resources.pri";
            if (std::filesystem::is_regular_file(index))
                if (const auto value = pri::resolve(
                        pri::strings(host::read_snapshot(index, pri::limit)), reference);
                    value && !value->empty()) {
                    info["display_name_reference"] = reference;
                    info["display_name"] = *value;
                }
        } catch (const std::exception &) {
        }
    }
    // the Xbox Live title ID, which either kind of manifest has under a name of its own, as
    // eight hexadecimal digits in capitals
    auto id = info.contains("title_id") && info.at("title_id").is_string() ? info.at("title_id")
              : info.contains("xbox_live_title_id") ? info.at("xbox_live_title_id")
                                                    : nlohmann::json(nullptr);
    if (id.is_string()) {
        auto text = id.get<std::string>();
        for (auto &c : text)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        id = text;
    }
    info["title_id"] = std::move(id);
    return info;
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

namespace {
// the manifest of the title `path` stands for: an executable is the same title as the
// package it is in
nlohmann::json own_manifest(const std::filesystem::path &path) {
    std::filesystem::path directory;
    auto manifest = read_manifest(path, directory);
    if (manifest.is_null()) {
        const auto package = runtime::package_directory(path.parent_path());
        if (std::filesystem::is_regular_file(package / manifest_name) ||
            std::filesystem::is_regular_file(package / config_name))
            manifest = read_manifest(package, directory);
    }
    return manifest;
}
} // namespace

std::string title_name(const std::filesystem::path &path) {
    try {
        const auto manifest = own_manifest(path);
        if (manifest.is_object() && manifest.at("display_name").is_string())
            if (const auto name = manifest.at("display_name").get<std::string>();
                !name.empty() && !name.starts_with("ms-resource:"))
                return name;
    } catch (const std::exception &) {
    }
    std::error_code error;
    const auto file = std::filesystem::is_directory(path, error) ? path.filename() : path.stem();
    const auto name = (file.empty() ? path.parent_path().filename() : file).u8string();
    return {name.begin(), name.end()};
}

std::string title_id(const std::filesystem::path &path) {
    try {
        const auto manifest = own_manifest(path);
        if (manifest.is_object() && manifest.at("title_id").is_string())
            return manifest.at("title_id").get<std::string>();
    } catch (const std::exception &) {
    }
    return {};
}

std::filesystem::path title_save_directory(const std::filesystem::path &path) {
    const auto manifest = own_manifest(path);
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
            " to the console system modules it came with and " + std::to_string(missing) +
            " have no implementation";
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

nlohmann::json title_properties(const std::filesystem::path &path) {
    const auto text = [](const std::filesystem::path &value) {
        const auto utf8 = value.u8string();
        return std::string(utf8.begin(), utf8.end());
    };
    nlohmann::json report{{"path", text(path)}, {"errors", nlohmann::json::object()}};
    // each part on its own: what one cannot tell does not hide the others
    const auto part = [&](const char *name, const auto &work) {
        try {
            work();
            return true;
        } catch (const std::exception &e) {
            report["errors"][name] = e.what();
            return false;
        }
    };
    std::filesystem::path file = path;
    part("manifest", [&] {
        std::filesystem::path directory;
        const auto manifest = read_manifest(path, directory);
        if (manifest.is_null())
            return;
        report["manifest"] = manifest;
        report["manifest_directory"] = text(directory);
        file = executable_of(manifest, directory);
    });
    report["executable"] = text(file);
    part("folders", [&] {
        report["save_directory"] = text(title_save_directory(path));
        report["system_directory"] = text(runtime::console_system_directory(file));
    });
    std::vector<std::uint8_t> bytes;
    nlohmann::json image, profile;
    if (!part("image", [&] {
            bytes = host::read_snapshot(file, 256 * 1024 * 1024);
            image = pe::inspect(bytes, pe::Detail::light);
            report["image"] = nlohmann::json::object();
            for (const auto *key :
                 {"format", "architecture", "machine", "image_base", "size_of_image",
                  "size_of_headers", "entry_rva", "section_alignment", "file_alignment",
                  "subsystem", "characteristics", "dll_characteristics"})
                if (image.contains(key))
                    report["image"][key] = image.at(key);
            report["image"]["file_size"] = bytes.size();
        }))
        return report;
    std::optional<Target> target;
    part("target", [&] {
        profile = analysis::import_profile(image);
        target = analysis::console_target(profile);
        report["target"] = target ? nlohmann::json(target_name(*target)) : nlohmann::json(nullptr);
        report["target_indication"] = profile.at("target_indication");
    });
    part("mapping", [&] {
        loader::MapOptions options;
        if (!image.at("directory_contents").contains("base_relocations"))
            options.guest_base = image.at("image_base").get<std::uint64_t>();
        const loader::MappedImage mapped(bytes, image, options);
        const auto &map = mapped.report();
        report["mapping"] = nlohmann::json::object();
        for (const auto *key : {"preferred_base", "guest_base", "size_of_image", "entry_va",
                                "relocation", "sections", "tls"})
            if (map.contains(key))
                report["mapping"][key] = map.at(key);
    });
    part("imports", [&] {
        const auto plan = runtime::bind_plan(
            bytes, target ? runtime::console_registry(*target) : runtime::title_registry(bytes),
            std::filesystem::absolute(file).parent_path(), runtime::console_system_directory(file));
        report["imports"] = plan.at("imports");
        // the modules in the order the image imports them, with what the profile calls each
        auto modules = nlohmann::json::array();
        std::map<std::string, std::size_t> index;
        const auto entry = [&](const std::string &name) -> nlohmann::json & {
            const auto [it, added] = index.try_emplace(name, modules.size());
            if (added)
                modules.push_back({{"module", name},
                                   {"category", nullptr},
                                   {"imports", 0},
                                   {"own", 0},
                                   {"host", 0},
                                   {"title", 0},
                                   {"system", 0},
                                   {"missing", 0}});
            return modules[it->second];
        };
        if (profile.is_object())
            for (const auto &module : profile.at("modules")) {
                auto &line = entry(module.at("module").get<std::string>());
                line["category"] = module.at("category");
                line["delay_load"] = module.at("delay_load");
            }
        for (const auto &import : plan.at("imports")) {
            auto &line = entry(import.at("module").get<std::string>());
            line["imports"] = line["imports"].get<int>() + 1;
            const auto &origin = import.at("origin").get_ref<const std::string &>();
            if (line.contains(origin) && line[origin].is_number())
                line[origin] = line[origin].get<int>() + 1;
        }
        report["modules"] = std::move(modules);
        for (const auto *key : {"own", "host", "title", "system", "missing"})
            report["bound"][key] = plan.at(key);
    });
    return report;
}

std::string exit_description(std::uint32_t code) {
    if (code == 0)
        return "The title ended";
    if (code == runtime::exit_unimplemented_import)
        return "The title called an import that is not implemented";
    if (code == runtime::exit_guest_fault)
        return "The title's code faulted";
    if (const auto name = host::status_text(code); !name.empty())
        return name;
    return "The title ended with a code of its own";
}
} // namespace xv
