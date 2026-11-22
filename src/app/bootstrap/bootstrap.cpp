#include "app/bootstrap/bootstrap.hpp"
#include "app/session.hpp"
#include "app/version.hpp"
#include "backends/graphics/backend.hpp"
#include "backends/input/input.hpp"
#include "backends/surface/surface.hpp"
#include "host/files.hpp"
#include "launcher/config.hpp"
#include "launcher/launch.hpp"
#include "launcher/library.hpp"
#include "runtime/console.hpp"
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace xv::bootstrap {
namespace {
std::wstring wide(const std::string &utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())).wstring();
}
// what config.ini says, the way the runtime takes it
runtime::ConsoleSettings console_settings() {
    const auto saved = load_console_settings();
    return {wide(saved.gamertag), wide(saved.xbox_user_id), saved.gamerscore,    saved.age_group,
            saved.headset,        saved.keyboard_dialog,    saved.system_storage};
}
// "<title> [<title ID>] | <video driver> | <n> FPS | XValkyrie <version> <commit>", with the frame
// rate over the time since it was last asked for. The driver is what the title's device was made
// on, and the backend selected until it has one.
struct Caption {
    using Clock = std::chrono::steady_clock;
    std::wstring name;
    Clock::time_point since = Clock::now();
    std::uint64_t frames = graphics::frames_presented();
    std::wstring operator()() {
        const auto now = Clock::now();
        const auto presented = graphics::frames_presented();
        const double seconds = std::chrono::duration<double>(now - since).count();
        const auto rate = seconds > 0 ? static_cast<double>(presented - frames) / seconds : 0;
        since = now;
        frames = presented;
        auto driver = graphics::video_driver();
        if (driver.empty())
            driver = graphics::selected_backend() == graphics::Backend::vulkan ? "Vulkan"
                                                                               : "Direct3D 11";
        auto text = name + L" | " + wide(driver) + L" | " +
                    std::to_wstring(static_cast<long long>(rate + .5)) + L" FPS | XValkyrie " +
                    wide(release);
        if (*commit)
            text += L" " + wide(commit);
        return text;
    }
};
// Commands on standard input, one a line, for driving a title without a person:
//   screenshot_temp                        the window's picture to xvboot_screenshot.bmp in
//                                          the temporary folder
//   joypad_press_key <control> [<ms>]      holds a control (a, b, dpad_down... as in
//                                          config.ini) for 150 ms or as long as said
void read_commands() {
    std::thread([] {
        for (std::string line; std::getline(std::cin, line);) {
            std::istringstream words(line);
            std::string command;
            if (!(words >> command))
                continue;
            if (command == "screenshot_temp") {
                const auto file = std::filesystem::temp_directory_path() / "xvboot_screenshot.bmp";
                std::cerr << "[Boot] screenshot_temp: "
                          << (surface::save_title_picture(file) ? app::text(file) : "failed")
                          << std::endl;
            } else if (command == "joypad_press_key") {
                std::string control;
                double milliseconds = 150;
                words >> control >> milliseconds;
                std::cerr << "[Boot] joypad_press_key " << control << ": "
                          << (input::press(control, milliseconds / 1000) ? "ok" : "unknown control")
                          << std::endl;
            } else {
                std::cerr << "[Boot] unknown command: " << command << std::endl;
            }
        }
    }).detach();
}
} // namespace

std::uint32_t run(const std::filesystem::path &path, runtime::ExecuteOptions options) {
    const auto file = title_executable(path);
    const auto bytes = host::read_snapshot(file, 256 * 1024 * 1024);
    options.save_directory = title_save_directory(path);
    options.shader_cache_directory = cache_directory() / "shaders";
    options.console = console_settings();
    // the title by its name, and by its title ID where it has one
    const auto id = title_id(path);
    const auto name = title_name(path) + (id.empty() ? "" : " [" + id + "]");
    surface::set_title_caption(Caption{wide(name)});
    if (const auto icon = title_icon(path); !icon.empty())
        surface::set_title_icon(icon);
    surface::use_exclusive_fullscreen(graphics::fullscreen_mode() == graphics::Fullscreen::native);
    // Closing its window ends the title. One that notices leaves by itself, with its own
    // exit code; the others, which is most of them, are ended a moment later.
    surface::on_title_closed([] {
        std::cerr << "[Boot] the title's window was closed" << std::endl;
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::cerr << "[Boot] the title is still running: ending it" << std::endl;
            host::end_now(0);
        }).detach();
    });
    read_commands();
    std::cerr << "[Boot] " << name << std::endl;
    std::cerr << "[Boot] starting " << app::text(file) << ", saves in "
              << app::text(options.save_directory) << std::endl;
    return runtime::execute(file, bytes, runtime::title_registry(bytes), options);
}
} // namespace xv::bootstrap
