#include "launcher/library.hpp"
#include "analysis/modules.hpp"
#include "formats/pe/inspect.hpp"
#include "formats/xvd/identify.hpp"
#include "host/files.hpp"
#include "launcher/launch.hpp"
#include "runtime/common/runtime.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string_view>
#include <utility>

namespace xv {
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
    // the largest of the sizes it comes in
    for (const auto *scale :
         {".scale-400", ".scale-200", ".scale-150", ".scale-125", ".scale-100"}) {
        if (const auto path = asset(directory, name.substr(0, dot) + scale + name.substr(dot));
            !path.empty())
            return path;
    }
    return {};
}
// width and height of a PNG, from its header; 0 for anything else
std::pair<std::uint32_t, std::uint32_t> picture_size(const std::filesystem::path &path) {
    unsigned char header[24] = {};
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char *>(header), sizeof header) ||
        std::string_view(reinterpret_cast<char *>(header), 8) != "\x89PNG\r\n\x1a\n" ||
        std::string_view(reinterpret_cast<char *>(header) + 12, 4) != "IHDR")
        return {0, 0};
    const auto number = [&](int at) {
        return std::uint32_t{header[at]} << 24 | std::uint32_t{header[at + 1]} << 16 |
               std::uint32_t{header[at + 2]} << 8 | header[at + 3];
    };
    return {number(16), number(20)};
}
// The pictures of a title, out of the ones its manifest names (`info` is what
// title_manifest gives; they are files under `directory`). A manifest says what each is
// for, not what it looks like, and packagers put anything anywhere, so the files decide:
// the cover is the largest picture that is about square, the icon the smallest one of at
// least 32 pixels. A picture whose size cannot be told counts as a small square. The
// store's logo is neither, unless the title has nothing else.
struct Pictures {
    std::filesystem::path cover, icon;
};
Pictures pictures(const std::filesystem::path &directory, const nlohmann::json &info) {
    struct Candidate {
        std::filesystem::path path;
        std::uint32_t side; // the shorter one
        bool square;
    };
    std::vector<Candidate> found;
    const auto add = [&](const nlohmann::json &name) {
        if (!name.is_string())
            return;
        const auto path = logo_asset(directory, name.get<std::string>());
        if (path.empty() || std::any_of(found.begin(), found.end(),
                                        [&](const auto &other) { return other.path == path; }))
            return;
        const auto [width, height] = picture_size(path);
        const auto low = std::min(width, height), high = std::max(width, height);
        found.push_back({path, std::max(low, 1u), high <= low + low / 4});
    };
    if (info.contains("logos") && info.at("logos").is_array())
        for (const auto &name : info.at("logos"))
            add(name);
    else if (info.contains("logo"))
        add(info.at("logo"));
    if (found.empty() && info.contains("store_logo"))
        add(info.at("store_logo"));
    Pictures result;
    const Candidate *cover = nullptr, *icon = nullptr;
    for (const auto &candidate : found) {
        if (!cover ||
            std::pair(candidate.square, candidate.side) > std::pair(cover->square, cover->side))
            cover = &candidate;
        if (candidate.square && candidate.side >= 32 && (!icon || candidate.side < icon->side))
            icon = &candidate;
    }
    if (cover)
        result.cover = cover->path;
    result.icon = icon ? icon->path : result.cover;
    return result;
}
// the file that describes the title in `directory`, empty if it has none
std::filesystem::path manifest_file(const std::filesystem::path &directory) {
    std::error_code error;
    for (const auto &folder : {directory, directory / "Mount"})
        for (const auto *name : {"MicrosoftGame.config", "AppxManifest.xml"})
            if (std::filesystem::is_regular_file(folder / name, error))
                return folder / name;
    return {};
}
bool manifest_name(const std::filesystem::path &path) {
    auto name = path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name == "appxmanifest.xml" || name == "microsoftgame.config";
}
std::optional<LibraryTitle> inspect(const std::filesystem::path &path) {
    try {
        if (std::filesystem::is_directory(path)) {
            const auto manifest = manifest_file(path);
            if (manifest.empty())
                return {};
            std::filesystem::path folder;
            const auto info = title_manifest(path, folder);
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
            // A name that is a reference into the package's resources cannot be read here:
            // the name of the package, without its publisher, says more than the folder.
            if (title.name.empty() || title.name.starts_with("ms-resource:")) {
                const auto dot = title.identity.find('.');
                title.name = title.identity.empty()            ? utf8(path.filename())
                             : dot + 1 < title.identity.size() ? title.identity.substr(dot + 1)
                                                               : title.identity;
            }
            title.version =
                info.at("version").is_string() ? info.at("version").get<std::string>() : "";
            if (info.at("title_id").is_string())
                title.title_id = info.at("title_id").get<std::string>();
            const auto chosen = pictures(manifest.parent_path(), info);
            title.logo = chosen.cover;
            title.icon = chosen.icon;
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
constexpr int cache_format = 4;
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
    stamp_file(manifest_file(title.path), result.data());
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
            title.icon = path_of(entry.at("icon"));
            title.splash = path_of(entry.at("splash"));
            title.name = entry.at("name").get<std::string>();
            title.identity = entry.at("identity").get<std::string>();
            title.title_id = entry.at("title_id").get<std::string>();
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
                            {"icon", utf8(title.icon)},
                            {"splash", utf8(title.splash)},
                            {"name", title.name},
                            {"identity", title.identity},
                            {"title_id", title.title_id},
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
        const auto manifest = manifest_file(directory);
        if (manifest.empty())
            return {};
        std::filesystem::path folder;
        const auto info = title_manifest(directory, folder);
        return info.at("splash").is_string()
                   ? logo_asset(manifest.parent_path(), info.at("splash").get<std::string>())
                   : std::filesystem::path{};
    } catch (const std::exception &) {
        return {};
    }
}
std::filesystem::path title_icon(const std::filesystem::path &path) {
    try {
        std::error_code error;
        auto directory = std::filesystem::is_directory(path, error) ? path : path.parent_path();
        // an executable deep inside its package
        if (manifest_file(directory).empty())
            directory = runtime::package_directory(directory);
        const auto manifest = manifest_file(directory);
        if (manifest.empty())
            return {};
        std::filesystem::path folder;
        return pictures(manifest.parent_path(), title_manifest(directory, folder)).icon;
    } catch (const std::exception &) {
        return {};
    }
}
void forget_cached_title(const std::filesystem::path &cache_file,
                         const std::filesystem::path &title) {
    auto cache = read_cache(cache_file);
    if (!cache.erase(title))
        return;
    std::vector<Cached> rest;
    for (auto &[path, entry] : cache)
        rest.push_back(std::move(entry));
    write_cache(cache_file, rest);
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
        auto title = xv::inspect(path);
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
            const auto candidate =
                it->is_directory(error) || manifest_name(path) || path.filename() == "Mount";
            if (error) {
                error.clear();
                continue;
            }
            // A title directory is a unit; do not index its Mount tree a second time.
            const auto title_path =
                manifest_name(path) || path.filename() == "Mount" ? path.parent_path() : path;
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
} // namespace xv
