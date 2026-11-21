#include "backends/input/input.hpp"
#include "backends/surface/surface.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <windows.h>

namespace xv::surface {
// The thread that owns a title window and runs its message loop.
struct TitleWindow::Thread {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable changed;
    HWND handle = nullptr;
    bool failed = false, closed = false;
};
namespace {
constexpr wchar_t class_name[] = L"XValkyrieTitle";
constexpr UINT_PTR caption_timer = 1;
std::atomic<bool> shown{false};
std::atomic<HWND> latest{nullptr};
std::mutex caption_mutex;
std::function<std::wstring()> caption;
std::function<void()> closed;

std::function<std::wstring()> current_caption() {
    const std::lock_guard lock(caption_mutex);
    return caption;
}
std::function<void()> current_closed() {
    const std::lock_guard lock(caption_mutex);
    return closed;
}

// Full screen. `exclusive_wanted` is what whoever presents goes by; it never waits for
// this thread, nor this thread for it.
constexpr UINT go_borderless = WM_APP + 1;
std::atomic<bool> exclusive_mode{false}, exclusive_offered{false}, exclusive_wanted{false};
// the window that is borderless, and what it was like before; the window's thread only
struct {
    HWND window = nullptr;
    LONG_PTR style = 0;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
} windowed;
void set_borderless(HWND handle, bool on) {
    if (on == (windowed.window == handle))
        return;
    if (!on) {
        SetWindowLongPtrW(handle, GWL_STYLE, windowed.style);
        SetWindowPlacement(handle, &windowed.placement);
        SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                         SWP_FRAMECHANGED);
        windowed.window = nullptr;
        return;
    }
    MONITORINFO monitor{sizeof monitor};
    if (!GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor))
        return;
    windowed.style = GetWindowLongPtrW(handle, GWL_STYLE);
    GetWindowPlacement(handle, &windowed.placement);
    windowed.window = handle;
    SetWindowLongPtrW(handle, GWL_STYLE, (windowed.style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    const auto &area = monitor.rcMonitor;
    SetWindowPos(handle, HWND_TOP, area.left, area.top, area.right - area.left,
                 area.bottom - area.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}
void toggle_fullscreen(HWND handle) {
    if (windowed.window == handle)
        set_borderless(handle, false);
    else if (exclusive_mode && exclusive_offered)
        exclusive_wanted = !exclusive_wanted;
    else
        set_borderless(handle, true);
}

LRESULT CALLBACK procedure(HWND handle, UINT message, WPARAM wide, LPARAM value) {
    switch (message) {
    case WM_CLOSE:
        ShowWindow(handle, SW_HIDE);
        PostQuitMessage(0);
        if (const auto notify = current_closed())
            notify();
        return 0;
    case WM_TIMER:
        if (wide == caption_timer) {
            if (const auto text = current_caption())
                SetWindowTextW(handle, text().c_str());
            return 0;
        }
        break;
    case go_borderless:
        set_borderless(handle, true);
        return 0;
    // the display goes back to the desktop when another program comes to the front
    case WM_ACTIVATEAPP:
        if (!wide)
            exclusive_wanted = false;
        break;
    // window capture
    case WM_KEYDOWN:
        if (wide == VK_F11) {
            if (!(value & (LPARAM{1} << 30))) // not while it is held
                toggle_fullscreen(handle);
            return 0;
        }
        [[fallthrough]];
    case WM_SYSKEYDOWN:
        input::window_key(static_cast<int>(wide), true);
        break;
    case WM_KEYUP:
        if (wide == VK_F11)
            return 0;
        [[fallthrough]];
    case WM_SYSKEYUP:
        input::window_key(static_cast<int>(wide), false);
        break;
    case WM_KILLFOCUS:
        input::window_focus_lost();
        break;
    case WM_LBUTTONDOWN:
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

void set_title_caption(std::function<std::wstring()> text) {
    const std::lock_guard lock(caption_mutex);
    caption = std::move(text);
}
void on_title_closed(std::function<void()> notify) {
    const std::lock_guard lock(caption_mutex);
    closed = std::move(notify);
}
void use_exclusive_fullscreen(bool exclusive) { exclusive_mode = exclusive; }
void offer_exclusive_fullscreen() { exclusive_offered = true; }
bool exclusive_fullscreen_wanted() { return exclusive_wanted; }
void exclusive_fullscreen_failed() {
    exclusive_offered = false;
    if (exclusive_wanted.exchange(false))
        if (const auto handle = latest.load())
            PostMessageW(handle, go_borderless, 0, 0);
}

TitleWindow::TitleWindow(const std::wstring &title, int width, int height) {
    register_class();
    thread_ = new Thread;
    thread_->thread = std::thread([state = thread_, title, width, height] {
        RECT frame{0, 0, width, height};
        AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
        const auto text = current_caption();
        const auto handle = CreateWindowExW(0, class_name, (text ? text() : title).c_str(),
                                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                            frame.right - frame.left, frame.bottom - frame.top,
                                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (handle)
            SetTimer(handle, caption_timer, 1000, nullptr);
        if (auto *const icon = detail::title_icon(); handle && icon)
            for (const auto size : {ICON_BIG, ICON_SMALL})
                SendMessageW(handle, WM_SETICON, size, reinterpret_cast<LPARAM>(icon));
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
    latest = thread_->handle;
}
void *detail::title_owner() {
    const auto handle = latest.load();
    return handle ? GetAncestor(handle, GA_ROOT) : nullptr;
}
TitleWindow::~TitleWindow() {
    shown = false;
    PostThreadMessageW(GetThreadId(thread_->thread.native_handle()), WM_QUIT, 0, 0);
    thread_->thread.join();
    delete thread_;
}
void TitleWindow::show() {
    if (!shown.exchange(true)) {
        ShowWindowAsync(static_cast<HWND>(handle_), SW_SHOW);
        SetForegroundWindow(static_cast<HWND>(handle_));
    }
}
bool TitleWindow::open() const {
    const std::lock_guard lock(thread_->mutex);
    return !thread_->closed;
}
void TitleWindow::wait_closed() const {
    std::unique_lock lock(thread_->mutex);
    thread_->changed.wait(lock, [&] { return thread_->closed; });
}
std::string TitleWindow::describe() const {
    const auto handle = static_cast<HWND>(handle_);
    RECT client{};
    GetClientRect(handle, &client);
    POINT origin{0, 0};
    ClientToScreen(handle, &origin);
    char text[96];
    std::snprintf(text, sizeof text, "%ldx%ld at %ld,%ld%s", client.right, client.bottom, origin.x,
                  origin.y, IsWindowVisible(handle) ? ", visible" : ", hidden");
    return text;
}
} // namespace xv::surface
