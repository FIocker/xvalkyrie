#include "app/library.hpp"
#include "app/launch.hpp"
#include "compatibility/modules.hpp"
#include "formats/appx/manifest.hpp"
#include "formats/pe/inspect.hpp"
#include "formats/xvd/identify.hpp"
#include "platform/host/files.hpp"
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <set>

namespace xs {
namespace {
std::string utf8(const std::filesystem::path &path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}
std::filesystem::path asset(const std::filesystem::path &directory, std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    const auto path = std::filesystem::path(std::u8string(name.begin(), name.end()));
    if (path.is_absolute() || name.find(':') != std::string::npos)
        return {};
    for (const auto &part : path)
        if (part == "..")
            return {};
    const auto full = directory / path;
    return std::filesystem::is_regular_file(full) && !std::filesystem::is_symlink(full)
               ? full
               : std::filesystem::path{};
}
std::filesystem::path logo_asset(const std::filesystem::path &directory, const std::string &name) {
    if (const auto path = asset(directory, name); !path.empty())
        return path;
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    for (const auto *scale : {".scale-100", ".scale-200", ".scale-400"}) {
        if (const auto path = asset(directory, name.substr(0, dot) + scale + name.substr(dot));
            !path.empty())
            return path;
    }
    return {};
}
std::optional<LibraryTitle> inspect(const std::filesystem::path &path) {
    try {
        if (std::filesystem::is_directory(path)) {
            auto manifest = path / "AppxManifest.xml";
            if (!std::filesystem::is_regular_file(manifest))
                manifest = path / "Mount" / "AppxManifest.xml";
            if (!std::filesystem::is_regular_file(manifest))
                return {};
            const auto info =
                appx::inspect_manifest(host::read_snapshot(manifest, appx::manifest_limit));
            const auto file = title_executable(path);
            if (!std::filesystem::is_regular_file(file) || std::filesystem::is_symlink(file))
                return {};
            LibraryTitle title;
            title.path = path;
            title.executable = file;
            title.identity = info.at("name").is_string() ? info.at("name").get<std::string>() : "";
            title.name = info.at("display_name").is_string()
                             ? info.at("display_name").get<std::string>()
                             : title.identity;
            if (title.name.empty() || title.name.starts_with("ms-resource:"))
                title.name = utf8(path.filename());
            title.version =
                info.at("version").is_string() ? info.at("version").get<std::string>() : "";
            if (info.at("logo").is_string())
                title.logo = logo_asset(manifest.parent_path(), info.at("logo").get<std::string>());
            const auto pe = pe::inspect(host::read_snapshot(file, 256 * 1024 * 1024));
            title.target = compat::console_target(compat::import_profile(pe));
            return title;
        }
        if (!std::filesystem::is_regular_file(path))
            return {};
        auto ext = utf8(path.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".xvd" && ext != ".xvc")
            return {};
        const auto prefix = host::read_prefix(path, xvd::header_size);
        const auto info = xvd::identify(prefix.bytes, prefix.file_size);
        LibraryTitle title;
        title.path = path;
        title.package = true;
        title.name = utf8(path.stem());
        title.identity = info.at("product_id_bytes").get<std::string>();
        const auto &platform = info.at("platform_mask");
        if (platform.at("gen9") == true && platform.at("gen8") == false)
            title.target = Target::scarlett;
        else if (platform.at("gen8") == true && platform.at("gen9") == false)
            title.target = Target::durango;
        return title;
    } catch (const std::exception &) {
        return {}; // malformed titles must not hide the rest of the library
    }
}
} // namespace

std::optional<LibraryTitle> discover_title(const std::filesystem::path &path) {
    return inspect(path);
}
std::vector<LibraryTitle> discover_library(const std::vector<std::filesystem::path> &roots) {
    std::vector<LibraryTitle> titles;
    std::set<std::filesystem::path> seen;
    for (const auto &root : roots) {
        std::error_code error;
        if (const auto title = inspect(root)) {
            const auto canonical = std::filesystem::weakly_canonical(root, error);
            if (!error && seen.insert(canonical).second)
                titles.push_back(*title);
            if (!title->package)
                continue;
        }
        if (!std::filesystem::is_directory(root, error) || error)
            continue;
        std::filesystem::recursive_directory_iterator it(
            root, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        std::size_t count = 0;
        for (; it != end && count++ < 10000; it.increment(error)) {
            if (error) {
                error.clear();
                continue;
            }
            if (it.depth() >= 5)
                it.disable_recursion_pending();
            if (it->is_symlink(error) || error) {
                error.clear();
                it.disable_recursion_pending();
                continue;
            }
            const auto path = it->path();
            const auto candidate = it->is_directory(error) ||
                                   path.filename() == "AppxManifest.xml" ||
                                   path.filename() == "Mount";
            if (error) {
                error.clear();
                continue;
            }
            // A title directory is a unit; do not index its Mount tree a second time.
            const auto title_path = path.filename() == "AppxManifest.xml" ? path.parent_path()
                                    : path.filename() == "Mount"          ? path.parent_path()
                                                                          : path;
            if (const auto title = inspect(title_path)) {
                const auto canonical = std::filesystem::weakly_canonical(title_path, error);
                if (!error && seen.insert(canonical).second)
                    titles.push_back(*title);
                if (it->is_directory(error))
                    it.disable_recursion_pending();
                error.clear();
            } else if (!candidate) {
                if (const auto package = inspect(path)) {
                    const auto canonical = std::filesystem::weakly_canonical(path, error);
                    if (!error && seen.insert(canonical).second)
                        titles.push_back(*package);
                    error.clear();
                }
            }
        }
    }
    std::sort(titles.begin(), titles.end(),
              [](const auto &a, const auto &b) { return a.name < b.name; });
    return titles;
}
} // namespace xs
