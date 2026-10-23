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
    CHECK(text.find("[graphics]\nbackend = vulkan\n") != std::string::npos);
    xs::load_graphics_settings();
    CHECK(xs::graphics::selected_backend() == xs::graphics::Backend::vulkan);
}
