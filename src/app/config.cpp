#include "app/config.hpp"
#include "input/input.hpp"
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace xs {
std::filesystem::path config_file() {
#ifdef _WIN32
    const auto *const base = _wgetenv(L"APPDATA");
    const auto folder = base ? std::filesystem::path(base) : std::filesystem::current_path();
    return folder / "XScarlet" / "settings.json";
#else
    const auto *const base = std::getenv("XDG_CONFIG_HOME");
    const auto *const home = std::getenv("HOME");
    const auto folder = base && *base ? std::filesystem::path(base)
                        : home        ? std::filesystem::path(home) / ".config"
                                      : std::filesystem::current_path();
    return folder / "xscarlet" / "settings.json";
#endif
}

void load_input_settings() {
    auto settings = input::default_settings();
    try {
        std::ifstream file(config_file());
        if (file) {
            const auto document = nlohmann::json::parse(file);
            if (document.is_object() && document.contains("input"))
                settings = input::from_json(document.at("input"));
        }
    } catch (const std::exception &) {
        // a broken file is the same as none
    }
    if (settings.capture == input::Capture::direct && !input::direct_capture_available())
        settings.capture = input::Capture::window;
    input::configure(settings);
}

void save_input_settings() {
    const auto path = config_file();
    auto document = nlohmann::json::object();
    try {
        std::ifstream file(path);
        if (file)
            document = nlohmann::json::parse(file);
    } catch (const std::exception &) {
    }
    if (!document.is_object())
        document = nlohmann::json::object();
    document["input"] = input::to_json(input::settings());
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::trunc);
    file << document.dump(2) << '\n';
    if (!file)
        throw std::runtime_error("Cannot write " + path.string());
}
} // namespace xs
