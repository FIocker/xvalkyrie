#include "runtime/modules/window.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/common/runtime.hpp"
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <windows.h>

namespace xv::runtime::modules {
namespace {
std::mutex mutex;
std::set<HWND> title_windows;

// The title's window exists, so that its messages and its window procedure work, and stays
// hidden.
HWND remember(HWND window) {
    if (window) {
        const std::lock_guard lock(mutex);
        title_windows.insert(window);
    }
    return window;
}
// The console has no frame around a title's window: the size the title asks for is the size
// of what it draws to. A top-level window gets no caption or border here either.
DWORD console_style(DWORD style, HWND parent) {
    style &= ~WS_VISIBLE;
    if (!parent && !(style & WS_CHILD))
        style = (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP;
    return style;
}
HWND WINAPI create_window_ex_w(DWORD extended, const wchar_t *class_name, const wchar_t *name,
                               DWORD style, int x, int y, int width, int height, HWND parent,
                               HMENU menu, HINSTANCE instance, void *parameter) {
    return remember(CreateWindowExW(extended, class_name, name, console_style(style, parent), x, y,
                                    width, height, parent, menu, instance, parameter));
}
HWND WINAPI create_window_ex_a(DWORD extended, const char *class_name, const char *name,
                               DWORD style, int x, int y, int width, int height, HWND parent,
                               HMENU menu, HINSTANCE instance, void *parameter) {
    const auto window = CreateWindowExA(extended, class_name, name, console_style(style, parent),
                                        x, y, width, height, parent, menu, instance, parameter);
    if (!window)
        std::fprintf(stderr, "[Runtime] CreateWindowExA(%s, style 0x%lx, %dx%d): error %lu\n",
                     IS_INTRESOURCE(class_name) ? "(atom)" : class_name, style, width, height,
                     GetLastError());
    return remember(window);
}
BOOL WINAPI show_window(HWND window, int command) {
    {
        const std::lock_guard lock(mutex);
        if (title_windows.contains(window))
            return FALSE;
    }
    return ShowWindow(window, command);
}
} // namespace

surface::TitleWindow &title_window() {
    static surface::TitleWindow window = [] {
        std::error_code error;
        auto name = std::filesystem::current_path(error).filename().wstring();
        return surface::TitleWindow(name.empty() ? L"XValkyrie" : name, 1280, 720);
    }();
    return window;
}

void install_window(Registry &registry) {
    registry.add_api("CreateWindowExW", reinterpret_cast<void *>(&create_window_ex_w));
    registry.add_api("CreateWindowExA", reinterpret_cast<void *>(&create_window_ex_a));
    registry.add_api("ShowWindow", reinterpret_cast<void *>(&show_window));
}
} // namespace xv::runtime::modules
