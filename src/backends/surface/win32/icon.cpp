#include "backends/surface/surface.hpp"
#include <algorithm>
#include <atomic>
#include <windows.h>
// GDI+ wants the COM declarations and the min and max macros, neither of which a lean
// windows.h without them gives it
#include <objidl.h>
namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

namespace xv::surface {
namespace {
std::atomic<HICON> icon{nullptr};
}

void set_title_icon(const std::filesystem::path &picture) {
    const Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
        return;
    {
        // the picture, whatever its shape, in the middle of a square the host scales down
        constexpr int side = 128;
        Gdiplus::Bitmap source(picture.c_str());
        const auto width = static_cast<int>(source.GetWidth()),
                   height = static_cast<int>(source.GetHeight());
        if (source.GetLastStatus() == Gdiplus::Ok && width > 0 && height > 0) {
            Gdiplus::Bitmap square(side, side, PixelFormat32bppARGB);
            Gdiplus::Graphics graphics(&square);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
            const int w = width >= height ? side : side * width / height,
                      h = height >= width ? side : side * height / width;
            graphics.DrawImage(&source, (side - w) / 2, (side - h) / 2, w, h);
            HICON made = nullptr;
            if (square.GetHICON(&made) == Gdiplus::Ok)
                icon = made;
        }
    }
    Gdiplus::GdiplusShutdown(token);
}
void *detail::title_icon() { return icon.load(); }
} // namespace xv::surface
