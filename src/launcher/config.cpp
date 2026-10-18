#include "launcher/config.hpp"
#include "backends/input/input.hpp"
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace xs {
std::filesystem::path config_directory() {
#ifdef _WIN32
    const auto *const base = _wgetenv(L"APPDATA");
    const auto *const home = _wgetenv(L"USERPROFILE");
    const auto folder = base && *base   ? std::filesystem::path(base)
                        : home && *home ? std::filesystem::path(home)
                                        : std::filesystem::current_path();
    return folder / "xscarlet";
#else
    const auto *const base = std::getenv("XDG_CONFIG_HOME");
    const auto *const home = std::getenv("HOME");
    const auto folder = base && *base   ? std::filesystem::path(base)
                        : home && *home ? std::filesystem::path(home) / ".config"
                                        : std::filesystem::current_path();
    return folder / "xscarlet";
#endif
}
std::filesystem::path config_file() { return config_directory() / "config.ini"; }
std::filesystem::path cache_directory() { return config_directory() / "cache"; }
std::filesystem::path library_cache_file() { return cache_directory() / "library.json"; }

namespace {
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}
std::string utf8(const std::filesystem::path &path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}
struct Settings {
    input::Settings input = input::default_settings();
    LibrarySettings library;
};
Settings read() {
    Settings result;
    std::ifstream file(config_file());
    if (!file) {
        // Existing installations used JSON; keep their controls when upgrading.
        try {
            std::ifstream old(config_directory() / "settings.json");
            if (old) {
                const auto json = nlohmann::json::parse(old);
                if (json.is_object() && json.contains("input"))
                    result.input = input::from_json(json.at("input"));
            }
        } catch (const std::exception &) {
        }
        return result;
    }
    std::string line, section;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const auto equal = line.find('=');
        if (equal == std::string::npos)
            continue;
        const auto key = trim(line.substr(0, equal)), value = trim(line.substr(equal + 1));
        if (section == "input") {
            if (key == "capture")
                result.input.capture =
                    value == "direct" ? input::Capture::direct : input::Capture::window;
            else if (key == "host_gamepad")
                result.input.host_gamepad = value != "false";
            else
                for (std::size_t i = 0; i < input::control_count; ++i)
                    if (key == input::control_id(static_cast<input::Control>(i))) {
                        try {
                            std::size_t used = 0;
                            const auto code = std::stoi(value, &used);
                            if (used == value.size() && code >= 0 && code < 256)
                                result.input.keys[i] = code;
                        } catch (const std::exception &) {
                        }
                        break;
                    }
        } else if (section == "libraries") {
            if (key == "view")
                result.library.grid = value != "list";
            else if (key == "path") {
                try {
                    const auto path = nlohmann::json::parse(value).get<std::string>();
                    result.library.directories.emplace_back(
                        std::u8string(path.begin(), path.end()));
                } catch (const std::exception &) {
                }
            }
        }
    }
    return result;
}
void write(const Settings &settings) {
    const auto path = config_file();
    std::filesystem::create_directories(path.parent_path());
    const auto temp = path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::trunc);
        file << "[input]\ncapture = "
             << (settings.input.capture == input::Capture::direct ? "direct" : "window")
             << "\nhost_gamepad = " << (settings.input.host_gamepad ? "true" : "false") << '\n';
        for (std::size_t i = 0; i < input::control_count; ++i)
            file << input::control_id(static_cast<input::Control>(i)) << " = "
                 << settings.input.keys[i] << '\n';
        file << "\n[libraries]\nview = " << (settings.library.grid ? "grid" : "list") << '\n';
        for (const auto &directory : settings.library.directories)
            file << "path = " << nlohmann::json(utf8(directory)).dump() << '\n';
        if (!file)
            throw std::runtime_error("Cannot write " + temp);
    }
#ifdef _WIN32
    if (!MoveFileExW(std::filesystem::path(temp).c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace " + path.string());
#else
    std::filesystem::rename(temp, path);
#endif
}
} // namespace

void load_input_settings() {
    auto settings = read().input;
    if (settings.capture == input::Capture::direct && !input::direct_capture_available())
        settings.capture = input::Capture::window;
    input::configure(settings);
}
void save_input_settings() {
    auto settings = read();
    settings.input = input::settings();
    write(settings);
}
LibrarySettings load_library_settings() { return read().library; }
void save_library_settings(const LibrarySettings &library) {
    auto settings = read();
    settings.input = input::settings();
    settings.library = library;
    write(settings);
}
} // namespace xs
