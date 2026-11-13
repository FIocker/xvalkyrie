#include "runtime/common/runtime.hpp"
#include "formats/appx/manifest.hpp"
#include "analysis/modules.hpp"
#include "formats/pe/inspect.hpp"
#include "host/files.hpp"
#include "loader/image.hpp"
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <nlohmann/json.hpp>
#include <system_error>

namespace xs::runtime {
namespace {
std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c);
    });
    return result;
}
std::filesystem::path saves;
ConsoleSettings console;
} // namespace

const std::filesystem::path &save_directory() { return saves; }
void detail::set_save_directory(std::filesystem::path directory) { saves = std::move(directory); }
const ConsoleSettings &console_settings() { return console; }
void detail::set_console_settings(ConsoleSettings settings) { console = std::move(settings); }

void Registry::add(std::string_view module, std::string_view symbol, void *function) {
    if (module.empty() || symbol.empty() || !function)
        throw std::invalid_argument("Registry entries need a module, a symbol and a function");
    if (!entries_.emplace(std::pair{lower(module), std::string(symbol)}, function).second)
        throw std::invalid_argument("Duplicate registry entry");
}
void *Registry::find(std::string_view module, std::string_view symbol) const {
    const auto entry = entries_.find({lower(module), std::string(symbol)});
    return entry == entries_.end() ? nullptr : entry->second;
}

void Registry::add_api(std::string_view symbol, void *function) {
    if (symbol.empty() || !function)
        throw std::invalid_argument("Registry API entries need a symbol and a function");
    if (!apis_.emplace(std::string(symbol), function).second)
        throw std::invalid_argument("Duplicate registry API entry");
}
void *Registry::find_api(std::string_view symbol) const {
    const auto entry = apis_.find(symbol);
    return entry == apis_.end() ? nullptr : entry->second;
}

bool Registry::has(std::string_view module) const {
    const auto name = lower(module);
    const auto entry = entries_.lower_bound({name, std::string()});
    return entry != entries_.end() && entry->first.first == name;
}

void Registry::forward(std::string_view module, std::string_view host_module) {
    if (module.empty() || host_module.empty())
        throw std::invalid_argument("Registry forwards need a module and a host module");
    if (!forwards_.emplace(lower(module), lower(host_module)).second)
        throw std::invalid_argument("Duplicate registry forward");
}
std::string_view Registry::forwarded(std::string_view module) const {
    const auto entry = forwards_.find(lower(module));
    return entry == forwards_.end() ? std::string_view{} : entry->second;
}

detail::Binding detail::resolve(const Registry &registry, std::string_view module,
                                std::string_view symbol) {
    if (auto *const function = registry.find(module, symbol))
        return {function, "own"};
    // ours whatever module it is imported from, the host policy included
    if (auto *const function = registry.find_api(symbol))
        return {function, "own"};
    const auto forwarded = registry.forwarded(module);
    if (forwarded.empty() && !registry.open() && !host_policy_allows(module, symbol))
        return {nullptr, "missing"};
    if (auto *const function = runtime_override(symbol))
        return {function, "own"};
    if (auto *const function = host_export(forwarded.empty() ? module : forwarded, symbol))
        return {function, "host"};
    return {nullptr, "missing"};
}

bool detail::host_policy_allows(std::string_view module, std::string_view symbol) {
    if (symbol.starts_with('#')) // can't check ordinals against the API list
        return false;
    const auto kind = analysis::classify_module(module);
    const std::string_view category = kind.category;
    if (category == "crt" || category == "os_crt")
        return true;
    return (category == "api_set" || category == "core_os" || category == "win32_legacy") &&
           analysis::is_game_os_api(symbol);
}

std::filesystem::path package_directory(const std::filesystem::path &directory) {
    std::error_code error;
    auto folder = directory;
    for (int depth = 0; depth < 8 && !folder.empty(); ++depth) {
        if (std::filesystem::is_regular_file(folder / "AppxManifest.xml", error) ||
            std::filesystem::is_regular_file(folder / "MicrosoftGame.config", error))
            return folder;
        if (folder == folder.parent_path())
            break;
        folder = folder.parent_path();
    }
    return directory;
}

std::filesystem::path console_system_directory(const std::filesystem::path &executable) {
    std::error_code error;
    const auto package =
        package_directory(std::filesystem::absolute(executable, error).parent_path());
    for (const auto &root : {package, package.parent_path()}) {
        const auto folder = root / "EmbeddedXvd" / "Windows" / "System32";
        if (std::filesystem::is_directory(folder, error))
            return folder;
    }
    return {};
}

bool console_system_module(std::string_view module) {
    const auto name = lower(module);
    return name == "xg_x.dll" || name == "xg.dll" || name == "xg_xs.dll";
}

namespace {
// The Name of the Identity element of the manifest in `folder`. Read as text: a framework
// package has no application, which the manifest reader insists on.
std::string package_name(const std::filesystem::path &folder) {
    try {
        const auto bytes = host::read_snapshot(folder / "AppxManifest.xml", appx::manifest_limit);
        const std::string_view xml(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        const auto identity = xml.find("<Identity");
        const auto end = xml.find('>', identity);
        if (identity == std::string_view::npos || end == std::string_view::npos)
            return {};
        const auto element = xml.substr(identity, end - identity);
        const auto name = element.find(" Name=\"");
        if (name == std::string_view::npos)
            return {};
        const auto value = element.substr(name + 7);
        return std::string(value.substr(0, value.find('"')));
    } catch (const std::exception &) {
        return {};
    }
}
// The packages the one in `root` depends on (its framework packages), where they are
// unpacked next to it: the folders beside `root` whose manifest has a name `root`'s lists.
// The manifest names a package as "Name.2.2"; the folder of an unpacked one is anything.
const std::vector<std::filesystem::path> &dependency_directories(
    const std::filesystem::path &package) {
    std::error_code absolute_error;
    auto root = std::filesystem::absolute(package, absolute_error).lexically_normal();
    if (!root.has_filename()) // "folder/." normalizes to "folder/"
        root = root.parent_path();
    static std::mutex mutex;
    static std::map<std::filesystem::path, std::vector<std::filesystem::path>> known;
    const std::lock_guard lock(mutex);
    if (const auto found = known.find(root); found != known.end())
        return found->second;
    auto &folders = known[root];
    try {
        const auto manifest = appx::inspect_manifest(
            host::read_snapshot(root / "AppxManifest.xml", appx::manifest_limit));
        std::set<std::string> wanted;
        for (const auto &name : manifest.at("dependencies"))
            wanted.insert(name.get<std::string>());
        std::error_code error;
        if (!wanted.empty())
            for (const auto &entry : std::filesystem::directory_iterator(root.parent_path(), error))
                if (entry.is_directory(error) && entry.path() != root &&
                    std::filesystem::is_regular_file(entry.path() / "AppxManifest.xml", error) &&
                    wanted.contains(package_name(entry.path())))
                    folders.push_back(entry.path());
    } catch (const std::exception &) {
        // a title without a manifest that can be read has no packages to look in
    }
    return folders;
}
} // namespace

std::filesystem::path detail::title_dll(const std::filesystem::path &directory,
                                        std::string_view module) {
    const auto name = lower(module);
    if (directory.empty() || name.empty() || name.find_first_of("\\/:") != std::string::npos)
        return {};
    const auto root = package_directory(directory);
    std::error_code error;
    for (auto folder = directory;; folder = folder.parent_path()) {
        if (std::filesystem::is_regular_file(folder / name, error))
            return folder / name;
        if (folder == root || folder == folder.parent_path())
            break;
    }
    for (const auto &folder : dependency_directories(root))
        if (std::filesystem::is_regular_file(folder / name, error))
            return folder / name;
    return {};
}

namespace {
// whether the DLL at `path` exports the symbol itself
bool exports(const std::filesystem::path &path, std::string_view symbol) {
    try {
        const auto report =
            pe::inspect(host::read_snapshot(path, 256 * 1024 * 1024), pe::Detail::light);
        const auto &contents = report.at("directory_contents");
        if (!(report.at("characteristics").get<std::uint64_t>() & 0x2000) ||
            !contents.contains("exports"))
            return false;
        for (const auto &exported : contents.at("exports").at("symbols")) {
            if (exported.contains("forwarder"))
                continue;
            if (symbol == "#" + std::to_string(exported.at("ordinal").get<std::uint64_t>()))
                return true;
            for (const auto &exported_name : exported.at("names"))
                if (exported_name.get_ref<const std::string &>() == symbol)
                    return true;
        }
    } catch (const std::exception &) {
        // not a DLL we can read: nothing it could provide
    }
    return false;
}
} // namespace

bool detail::title_provides(const Registry &registry, const std::filesystem::path &directory,
                            std::string_view module, std::string_view symbol) {
    if (directory.empty() || registry.find(module, symbol) || !registry.forwarded(module).empty())
        return false;
    const auto path = title_dll(directory, module);
    return !path.empty() && exports(path, symbol);
}

nlohmann::json bind_plan(std::span<const std::uint8_t> file, const Registry &registry,
                         const std::filesystem::path &directory,
                         const std::filesystem::path &system_directory) {
    using json = nlohmann::json;
    const auto contents = pe::inspect(file, pe::Detail::light).at("directory_contents");
    auto imports = json::array(), opaque = json::array(), delayed = json::array();
    std::size_t own = 0, host = 0, title = 0, system = 0, missing = 0;
    // a module of the console's system the title shipped with, which execute() loads first
    const auto from_system = [&](const std::string &module, std::string_view symbol) {
        std::error_code error;
        const auto path = system_directory / module;
        return !system_directory.empty() && console_system_module(module) &&
               std::filesystem::is_regular_file(path, error) && exports(path, symbol);
    };
    if (contents.contains("imports")) {
        for (const auto &entry : contents.at("imports")) {
            const auto &module = entry.at("module").get_ref<const std::string &>();
            if (!entry.contains("symbols")) {
                opaque.push_back(module);
                continue;
            }
            for (const auto &item : entry.at("symbols")) {
                const auto symbol = loader::import_label(item);
                const std::string_view origin =
                    from_system(module, symbol)                                   ? "system"
                    : detail::title_provides(registry, directory, module, symbol) ? "title"
                    : detail::resolve(registry, module, symbol).origin;
                ++(origin == "own"      ? own
                   : origin == "host"   ? host
                   : origin == "title"  ? title
                   : origin == "system" ? system
                                        : missing);
                imports.push_back({{"module", module}, {"symbol", symbol}, {"origin", origin}});
            }
        }
    }
    if (contents.contains("delay_imports"))
        for (const auto &entry : contents.at("delay_imports"))
            delayed.push_back(entry.at("module"));
    return {{"imports", std::move(imports)},
            {"own", own},
            {"host", host},
            {"title", title},
            {"system", system},
            {"missing", missing},
            {"modules_without_symbols", std::move(opaque)},
            {"delay_load_modules", std::move(delayed)}};
}
} // namespace xs::runtime
