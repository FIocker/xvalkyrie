#include "backends/surface/surface.hpp"
#include <cstdint>
#include <cstdio>
#include <vector>
#include <windows.h>

namespace xv::surface {
bool save_title_picture(const std::filesystem::path &file) {
    const auto window = static_cast<HWND>(detail::title_owner());
    RECT area{};
    if (!window || !GetClientRect(window, &area) || area.right <= 0 || area.bottom <= 0)
        return false;
    const int width = area.right, height = area.bottom;
    const auto screen = GetDC(window);
    const auto memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof info.bmiHeader, width, -height, 1, 32, BI_RGB};
    void *bits = nullptr;
    const auto bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool saved = false;
    if (memory && bitmap) {
        const auto before = SelectObject(memory, bitmap);
        // PW_CLIENTONLY | PW_RENDERFULLCONTENT: what a flip swap chain shows is not in the
        // window's own surface
        if (PrintWindow(window, memory, 1 | 2)) {
            GdiFlush();
            const auto bytes = static_cast<std::uint32_t>(width) * 4 * height;
            BITMAPFILEHEADER header{0x4d42, 54 + bytes, 0, 0, 54};
            if (auto *const out = _wfopen(file.c_str(), L"wb")) {
                saved = std::fwrite(&header, sizeof header, 1, out) == 1 &&
                        std::fwrite(&info.bmiHeader, sizeof info.bmiHeader, 1, out) == 1 &&
                        std::fwrite(bits, 1, bytes, out) == bytes;
                std::fclose(out);
            }
        }
        SelectObject(memory, before);
    }
    if (bitmap)
        DeleteObject(bitmap);
    if (memory)
        DeleteDC(memory);
    ReleaseDC(window, screen);
    return saved;
}
} // namespace xv::surface
