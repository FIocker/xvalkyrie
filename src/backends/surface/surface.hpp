#pragma once
#include <functional>
#include <optional>
#include <string>

namespace xs::surface {
struct TextRequest {
    std::wstring title, description, text;
    bool password = false, digits = false;
    unsigned max_length = 0; // 0: no limit
};
// A window over the title's; `done` runs on another thread, with nothing if cancelled.
void ask_text(TextRequest request, std::function<void(std::optional<std::wstring>)> done);
// Makes titles draw inside `parent`, a native window of the calling thread, instead of in
// a window of their own: creates the surface for them there, hidden. Call before a title
// starts, from the thread that runs the parent's message loop.
void set_title_parent(void *parent);
// The surface set_title_parent made, null before it: a native window another process can
// be given to draw into with use_title_surface. Hidden again by hide_title_surface, when
// that process is gone.
void *title_surface();
void use_title_surface(void *surface);
void hide_title_surface();
// the top-level window the surface is in, null without a surface
void *title_surface_owner();
// The part of the parent the surface covers, in pixels of its client area. Same thread.
void set_title_area(int x, int y, int width, int height);
// true while a title is showing frames, which starts with its first one
bool title_window_shown();
// The interface has something where the surface is (a menu, the settings), which a native
// child window would hide: the surface is hidden until this is called with false. The
// title keeps running. Same thread as set_title_parent.
void set_title_covered(bool covered);

// The window a title presents into: the surface inside the parent if there is one,
// otherwise a window of its own. Either way its messages are handled by another thread
// than the title's (the parent's, or one made for the window), so that dragging, resizing
// or a busy title never hold each other up.
class TitleWindow {
  public:
    TitleWindow(const std::wstring &title, int width, int height);
    ~TitleWindow();
    TitleWindow(const TitleWindow &) = delete;
    TitleWindow &operator=(const TitleWindow &) = delete;
    // Makes the window visible, without taking the keyboard focus from whatever has it.
    // Any thread; does nothing the second time.
    void show();
    // false once the user has closed the window
    bool open() const;
    // blocks until the window is closed
    void wait_closed() const;
    void *native() const { return handle_; } // HWND
    // where the window is and whether it can be seen, for the log
    std::string describe() const;

  private:
    void *handle_ = nullptr;
    bool owned_ = true;
    struct Thread;
    Thread *thread_ = nullptr;
};
namespace detail {
// HWND of the top-level window of the last title window
void *title_owner();
} // namespace detail
} // namespace xs::surface
