#pragma once
#include "app/ui/theme.hpp"
#include "launcher/config.hpp"
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace xs::ui {
// The settings window: a list of pages on the left, the chosen one on the right. A new
// page is an entry in `pages` (settings.cpp) and a function that draws it.
class Settings {
  public:
    enum class Page { input, graphics, libraries, about };
    // What the settings need done by whoever owns the window. All of them are called
    // from draw().
    struct Host {
        std::function<void()> add_folder;                 // ask for a folder to add
        std::function<void(std::size_t)> change_folder;   // ask for one to put in its place
        std::function<void()> rescan;                     // look for titles again
        std::function<void(bool rescan)> libraries_saved; // `libraries` was changed
        std::function<void()> input_saved;                // input::settings() was; may throw
        std::function<void()> graphics_saved;
    };
    // what the library page shows about the library itself
    struct Library {
        LibrarySettings &settings;
        std::size_t titles;
        bool scanning;
        const std::string &notice;
    };

    void show(Page page) {
        page_ = page;
        open_ = true;
        raise_ = true;
    }
    bool shown() const { return open_; }
    // While the input page is up the keyboard is for choosing keys, not for the title.
    bool input_shown() const { return open_ && page_ == Page::input; }
    // true while a control waits for the key to bind to it: give the next one to key()
    bool wants_key() const { return binding_.has_value(); }
    void key(int virtual_key) { pressed_ = virtual_key; }
    void draw(const Fonts &fonts, const Host &host, Library library);

  private:
    void input(const Fonts &fonts, const Host &host);
    void graphics(const Fonts &fonts, const Host &host);
    void libraries(const Fonts &fonts, const Host &host, Library library);
    void about(const Fonts &fonts);

    bool open_ = false, raise_ = false;
    Page page_ = Page::input;
    std::optional<std::size_t> binding_; // control waiting for a key
    int pressed_ = 0;                    // the key it got
    std::string input_notice_, graphics_notice_;
};
} // namespace xs::ui
