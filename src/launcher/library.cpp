#include "launcher/library.hpp"
#include "analysis/modules.hpp"
#include "formats/appx/manifest.hpp"
#include "formats/pe/inspect.hpp"
#include "formats/xvd/identify.hpp"
#include "host/files.hpp"
#include "launcher/launch.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
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
            if (info.at("splash").is_string())
                title.splash =
                    logo_asset(manifest.parent_path(), info.at("splash").get<std::string>());
            const auto pe =
                pe::inspect(host::FileMapping(file, 256 * 1024 * 1024).bytes(), pe::Detail::light);
            title.target = analysis::console_target(analysis::import_profile(pe));
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

// ---- the cache: what a scan found, and the size and time of the files it was read from
constexpr int cache_format = 1;
using Stamp = std::array<std::int64_t, 4>; // manifest or package size and time, executable's
void stamp_file(const std::filesystem::path &path, std::int64_t *into) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    into[0] = error ? -1 : static_cast<std::int64_t>(size);
    const auto time = std::filesystem::last_write_time(path, error);
    into[1] = error ? -1 : static_cast<std::int64_t>(time.time_since_epoch().count());
}
// What the title read from these files depends on, without reading them.
Stamp stamp(const LibraryTitle &title) {
    Stamp result{-1, -1, -1, -1};
    if (title.package) {
        stamp_file(title.path, result.data());
        return result;
    }
    std::error_code error;
    auto manifest = title.path / "AppxManifest.xml";
    if (!std::filesystem::is_regular_file(manifest, error))
        manifest = title.path / "Mount" / "AppxManifest.xml";
    stamp_file(manifest, result.data());
    stamp_file(title.executable, result.data() + 2);
    return result;
}
std::filesystem::path path_of(const nlohmann::json &value) {
    const auto &text = value.get_ref<const std::string &>();
    return std::u8string(text.begin(), text.end());
}
struct Cached {
    LibraryTitle title;
    Stamp stamp;
};
using Cache = std::map<std::filesystem::path, Cached>;
Cache read_cache(const std::filesystem::path &file) {
    Cache cache;
    if (file.empty())
        return cache;
    try {
        std::ifstream input(file);
        if (!input)
            return cache;
        const auto json = nlohmann::json::parse(input);
        if (json.at("format") != cache_format)
            return cache;
        for (const auto &entry : json.at("titles")) {
            LibraryTitle title;
            title.path = path_of(entry.at("path"));
            title.executable = path_of(entry.at("executable"));
            title.logo = path_of(entry.at("logo"));
            title.splash = path_of(entry.at("splash"));
            title.name = entry.at("name").get<std::string>();
            title.identity = entry.at("identity").get<std::string>();
            title.version = entry.at("version").get<std::string>();
            title.package = entry.at("package").get<bool>();
            if (entry.at("target") == "durango")
                title.target = Target::durango;
            else if (entry.at("target") == "scarlett")
                title.target = Target::scarlett;
            const auto stamp = entry.at("stamp").get<Stamp>();
            auto key = title.path;
            cache.insert_or_assign(std::move(key), Cached{std::move(title), stamp});
        }
    } catch (const std::exception &) {
        cache.clear(); // a damaged cache is a cold one
    }
    return cache;
}
void write_cache(const std::filesystem::path &file, const std::vector<Cached> &titles) {
    if (file.empty())
        return;
    try {
        auto list = nlohmann::json::array();
        for (const auto &[title, stamp] : titles)
            list.push_back({{"path", utf8(title.path)},
                            {"executable", utf8(title.executable)},
                            {"logo", utf8(title.logo)},
                            {"splash", utf8(title.splash)},
                            {"name", title.name},
                            {"identity", title.identity},
                            {"version", title.version},
                            {"package", title.package},
                            {"target", title.target ? nlohmann::json(target_id(*title.target))
                                                    : nlohmann::json(nullptr)},
                            {"stamp", stamp}});
        std::filesystem::create_directories(file.parent_path());
        auto temporary = file;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            output << nlohmann::json{{"format", cache_format}, {"titles", list}}.dump();
            if (!output)
                return;
        }
        std::filesystem::rename(temporary, file);
    } catch (const std::exception &) {
        // without a cache the next scan is only slower
    }
}
bool under(const std::filesystem::path &path, const std::filesystem::path &root) {
    const auto a = path.lexically_normal(), b = root.lexically_normal();
    auto it = a.begin();
    for (const auto &part : b) {
        if (part.empty())
            continue; // the trailing separator of "C:/games/"
        if (it == a.end() || *it != part)
            return false;
        ++it;
    }
    return true;
}
} // namespace

std::optional<LibraryTitle> discover_title(const std::filesystem::path &path) {
    return inspect(path);
}
std::filesystem::path title_splash(const std::filesystem::path &path) {
    try {
        const auto directory = std::filesystem::is_directory(path) ? path : path.parent_path();
        auto manifest = directory / "AppxManifest.xml";
        if (!std::filesystem::is_regular_file(manifest))
            manifest = directory / "Mount" / "AppxManifest.xml";
        if (!std::filesystem::is_regular_file(manifest))
            return {};
        const auto info =
            appx::inspect_manifest(host::read_snapshot(manifest, appx::manifest_limit));
        return info.at("splash").is_string()
                   ? logo_asset(manifest.parent_path(), info.at("splash").get<std::string>())
                   : std::filesystem::path{};
    } catch (const std::exception &) {
        return {};
    }
}
std::vector<LibraryTitle> discover_library(const std::vector<std::filesystem::path> &roots,
                                           const std::filesystem::path &cache_file) {
    const auto cache = read_cache(cache_file);
    std::vector<Cached> found;
    // a title is read again only if its files changed since it went into the cache
    const auto inspect = [&](const std::filesystem::path &path) -> std::optional<Cached> {
        if (const auto it = cache.find(path); it != cache.end()) {
            if (const auto now = stamp(it->second.title); now == it->second.stamp)
                return it->second;
        }
        auto title = xs::inspect(path);
        if (!title)
            return {};
        auto stamped = stamp(*title);
        return Cached{std::move(*title), stamped};
    };
    std::set<std::filesystem::path> seen;
    for (const auto &root : roots) {
        std::error_code error;
        if (const auto title = inspect(root)) {
            const auto canonical = std::filesystem::weakly_canonical(root, error);
            if (!error && seen.insert(canonical).second)
                found.push_back(*title);
            if (!title->title.package)
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
                    found.push_back(*title);
                if (it->is_directory(error))
                    it.disable_recursion_pending();
                error.clear();
            } else if (!candidate) {
                if (const auto package = inspect(path)) {
                    const auto canonical = std::filesystem::weakly_canonical(path, error);
                    if (!error && seen.insert(canonical).second)
                        found.push_back(*package);
                    error.clear();
                }
            }
        }
    }
    std::sort(found.begin(), found.end(),
              [](const auto &a, const auto &b) { return a.title.name < b.title.name; });
    write_cache(cache_file, found);
    std::vector<LibraryTitle> titles;
    for (auto &entry : found)
        titles.push_back(std::move(entry.title));
    return titles;
}

std::vector<LibraryTitle> cached_library(const std::vector<std::filesystem::path> &roots,
                                         const std::filesystem::path &cache_file) {
    std::vector<LibraryTitle> titles;
    for (auto &[path, entry] : read_cache(cache_file)) {
        std::error_code error;
        if (std::any_of(roots.begin(), roots.end(),
                        [&](const auto &root) { return under(path, root); }) &&
            std::filesystem::exists(path, error))
            titles.push_back(std::move(entry.title));
    }
    std::sort(titles.begin(), titles.end(),
              [](const auto &a, const auto &b) { return a.name < b.name; });
    return titles;
}
} // namespace xs
