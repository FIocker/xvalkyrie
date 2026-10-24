#include "launcher/config.hpp"
#include "backends/graphics/backend.hpp"
#include "backends/input/input.hpp"
#include <catch2/catch.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

TEST_CASE("user configuration persists libraries and controls") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
                      ("xscarlet-config-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(root));
    const auto old_input = xs::input::settings();
    const auto *previous = _wgetenv(L"APPDATA");
    const std::wstring original = previous ? previous : L"";
    REQUIRE(_wputenv_s(L"APPDATA", root.c_str()) == 0);
    struct Restore {
        fs::path root;
        xs::input::Settings input;
        decltype(original) env;
        ~Restore() {
            xs::input::configure(input);
            _wputenv_s(L"APPDATA", env.c_str());
            fs::remove_all(root);
        }
    } restore{root, old_input, original};

    auto controls = xs::input::default_settings();
    controls.keys[static_cast<std::size_t>(xs::input::Control::a)] = 'K';
    xs::input::configure(controls);
    xs::save_input_settings();
    xs::input::configure(xs::input::default_settings());
    xs::load_input_settings();
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

    xs::graphics::select_backend(xs::graphics::Backend::vulkan);
    xs::save_graphics_settings();
    xs::save_library_settings(libraries);
    std::ifstream file(xs::config_file());
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    file.close();
    CHECK(text.find("[graphics]\nbackend = vulkan\n") != std::string::npos);
    xs::load_graphics_settings();
    CHECK(xs::graphics::selected_backend() == xs::graphics::Backend::vulkan);

    CHECK(xs::load_console_settings().gamertag == "Player");
    xs::ConsoleSettings console{"Señor = Cat", "123", 4500, 2, true, false};
    xs::save_console_settings(console);
    xs::save_library_settings(libraries);
    const auto saved = xs::load_console_settings();
    CHECK(saved.gamertag == console.gamertag);
    CHECK(saved.xbox_user_id == "123");
    CHECK(saved.gamerscore == 4500);
    CHECK(saved.age_group == 2);
    CHECK(saved.headset);
    CHECK_FALSE(saved.keyboard_dialog);
    std::ofstream(xs::config_file(), std::ios::trunc)
        << "[console]\ngamertag = \"a gamertag far too long\"\nxbox_user_id = 12a\n"
           "gamerscore = -1\nage_group = 9\nheadset = maybe\nkeyboard\n";
    const auto broken = xs::load_console_settings();
    CHECK(broken.gamertag == "Player");
    CHECK(broken.xbox_user_id == "2814600000000001");
    CHECK(broken.gamerscore == 0);
    CHECK(broken.age_group == 3);
    CHECK_FALSE(broken.headset);
    CHECK(broken.keyboard_dialog);
    CHECK_FALSE(xs::valid_gamertag(""));
    CHECK(xs::valid_gamertag("\xc3\xb1\xc3\xb1\xc3\xb1\xc3\xb1\xc3\xb1\xc3\xb1\xc3\xb1\xc3\xb1"));
}
