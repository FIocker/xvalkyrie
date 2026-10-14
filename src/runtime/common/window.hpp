#pragma once
#include <string>

namespace xs::runtime {
// Makes titles draw inside `parent`, a native window of the calling thread, instead of in
// a window of their own: creates the surface for them there, hidden. Call before a title
// starts, from the thread that runs the parent's message loop.
void set_title_parent(void *parent);
// The part of the parent the surface covers, in pixels of its client area. Same thread.
void set_title_area(int x, int y, int width, int height);
// true while a title is showing frames, which starts with its first one
bool title_window_shown();

// The window a title presents into: the surface inside the parent if there is one,
// otherwise a window of its own. Either way its messages are handled by another thread
// than the title's (the parent's, or one made for the window), so that dragging, resizing
// or a busy title never hold each other up. Only implemented on hosts that can execute.
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
} // namespace xs::runtime
