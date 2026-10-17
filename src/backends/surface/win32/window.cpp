#include "backends/input/input.hpp"
#include "backends/surface/surface.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <windows.h>

namespace xs::surface {
// The thread that owns a title window of its own and runs its message loop.
struct TitleWindow::Thread {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable changed;
    HWND handle = nullptr;
    bool failed = false, closed = false;
};
namespace {
constexpr wchar_t class_name[] = L"XScarletTitle";
std::atomic<HWND> surface{nullptr};
std::atomic<bool> shown{false};

LRESULT CALLBACK procedure(HWND handle, UINT message, WPARAM wide, LPARAM value) {
    switch (message) {
    case WM_CLOSE:
        ShowWindow(handle, SW_HIDE);
        PostQuitMessage(0);
        return 0;
    // window capture for a title window of its own; inside a parent the parent sees the keys
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        input::window_key(static_cast<int>(wide), true);
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        input::window_key(static_cast<int>(wide), false);
        break;
    case WM_KILLFOCUS:
        input::window_focus_lost();
        break;
    case WM_LBUTTONDOWN:
        if (!GetParent(handle))
            SetFocus(handle);
        break;
    }
    return DefWindowProcW(handle, message, wide, value);
}
void register_class() {
    WNDCLASSEXW description{sizeof description};
    description.lpfnWndProc = &procedure;
    description.hInstance = GetModuleHandleW(nullptr);
    description.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    description.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    description.lpszClassName = class_name;
    if (!RegisterClassExW(&description) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Cannot register the title window class");
}
} // namespace

void set_title_parent(void *window) {
    const auto parent = static_cast<HWND>(window);
    if (!parent || surface.load())
        return;
    register_class();
    // or the parent would paint over the title every time it redraws itself
    SetWindowLongPtrW(parent, GWL_STYLE, GetWindowLongPtrW(parent, GWL_STYLE) | WS_CLIPCHILDREN);
    surface = CreateWindowExW(0, class_name, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 16, 16, parent,
                              nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!surface.load())
        throw std::runtime_error("Cannot create the title surface");
}
void set_title_area(int x, int y, int width, int height) {
    if (const auto handle = surface.load())
        SetWindowPos(handle, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
bool title_window_shown() { return shown; }

TitleWindow::TitleWindow(const std::wstring &title, int width, int height) {
    if (const auto handle = surface.load()) {
        handle_ = handle;
        owned_ = false;
        return;
    }
    register_class();
    thread_ = new Thread;
    thread_->thread = std::thread([state = thread_, title, width, height] {
        RECT frame{0, 0, width, height};
        AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
        const auto handle =
            CreateWindowExW(0, class_name, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top,
                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        {
            const std::lock_guard lock(state->mutex);
            state->handle = handle;
            state->failed = !handle;
        }
        state->changed.notify_all();
        MSG message;
        while (handle && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (handle)
            DestroyWindow(handle);
        {
            const std::lock_guard lock(state->mutex);
            state->closed = true;
        }
        state->changed.notify_all();
    });
    std::unique_lock lock(thread_->mutex);
    thread_->changed.wait(lock, [&] { return thread_->handle || thread_->failed; });
    if (thread_->failed) {
        lock.unlock();
        thread_->thread.join();
        delete thread_;
        throw std::runtime_error("Cannot create the title window");
    }
    handle_ = thread_->handle;
}
TitleWindow::~TitleWindow() {
    shown = false;
    if (!owned_) {
        ShowWindowAsync(static_cast<HWND>(handle_), SW_HIDE);
        return;
    }
    PostThreadMessageW(GetThreadId(thread_->thread.native_handle()), WM_QUIT, 0, 0);
    thread_->thread.join();
    delete thread_;
}
void TitleWindow::show() {
    if (!shown.exchange(true))
        ShowWindowAsync(static_cast<HWND>(handle_), SW_SHOWNA);
}
bool TitleWindow::open() const {
    if (!owned_)
        return true;
    const std::lock_guard lock(thread_->mutex);
    return !thread_->closed;
}
void TitleWindow::wait_closed() const {
    if (!owned_) {
        Sleep(INFINITE); // the parent ends the process
        return;
    }
    std::unique_lock lock(thread_->mutex);
    thread_->changed.wait(lock, [&] { return thread_->closed; });
}
std::string TitleWindow::describe() const {
    const auto handle = static_cast<HWND>(handle_);
    RECT client{}, parent_client{};
    GetClientRect(handle, &client);
    POINT origin{0, 0};
    const auto parent = GetParent(handle);
    if (parent) {
        MapWindowPoints(handle, parent, &origin, 1);
        GetClientRect(parent, &parent_client);
    }
    char text[160];
    std::snprintf(text, sizeof text, "%ldx%ld at %ld,%ld %s%s", client.right, client.bottom,
                  origin.x, origin.y, parent ? "inside the main window" : "in a window of its own",
                  IsWindowVisible(handle) ? ", visible" : ", hidden");
    std::string result = text;
    if (parent)
        result += " (main window " + std::to_string(parent_client.right) + "x" +
                  std::to_string(parent_client.bottom) + ")";
    return result;
}
} // namespace xs::surface
