#include "launcher/config.hpp"
#include "backends/input/input.hpp"
#include <catch2/catch.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

TEST_CASE("user configuration persists libraries and controls") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
                      ("xscarlet-config-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(root));
    const auto old_input = xs::input::settings();
#ifdef _WIN32
    const auto *previous = _wgetenv(L"APPDATA");
    const std::wstring original = previous ? previous : L"";
    REQUIRE(_wputenv_s(L"APPDATA", root.c_str()) == 0);
#else
    const auto *previous = std::getenv("XDG_CONFIG_HOME");
    const std::string original = previous ? previous : "";
    REQUIRE(setenv("XDG_CONFIG_HOME", root.c_str(), 1) == 0);
#endif
    struct Restore {
        fs::path root;
        xs::input::Settings input;
        decltype(original) env;
        ~Restore() {
            xs::input::configure(input);
#ifdef _WIN32
            _wputenv_s(L"APPDATA", env.c_str());
#else
            if (env.empty())
                unsetenv("XDG_CONFIG_HOME");
            else
                setenv("XDG_CONFIG_HOME", env.c_str(), 1);
#endif
            fs::remove_all(root);
        }
    } restore{root, old_input, original};

    fs::create_directories(xs::config_directory());
    auto controls = xs::input::default_settings();
    controls.keys[static_cast<std::size_t>(xs::input::Control::a)] = 'K';
    std::ofstream(xs::config_directory() / "settings.json")
        << nlohmann::json{{"input", xs::input::to_json(controls)}}.dump();
    xs::load_input_settings(); // migrate old JSON without losing the binding
    CHECK(xs::input::settings().keys[static_cast<std::size_t>(xs::input::Control::a)] == 'K');

    xs::LibrarySettings libraries{{root / "My Games", root / "Other Games"}, false};
    xs::save_library_settings(libraries);
    CHECK(xs::config_file().filename() == "config.ini");
    const auto loaded = xs::load_library_settings();
    CHECK(loaded.directories == libraries.directories);
    CHECK_FALSE(loaded.grid);
    xs::input::configure(xs::input::default_settings());
    xs::load_input_settings();
    CHECK(xs::input::settings().keys[static_cast<std::size_t>(xs::input::Control::a)] == 'K');
    xs::save_input_settings();
    CHECK(xs::load_library_settings().directories == libraries.directories);
}
