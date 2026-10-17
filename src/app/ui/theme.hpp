#pragma once
#include <array>
#include <imgui.h>

// How the window looks: fonts, colors, and the things drawn by hand (backdrop, wordmark).
namespace xs::ui {
inline constexpr std::array<float, 6> pixel_sizes{26, 52, 78, 104, 156, 208};
struct Fonts {
    ImFont *body = nullptr;    // 16 px, the default
    ImFont *small = nullptr;   // 13 px: badges, secondary lines
    ImFont *heading = nullptr; // 24 px
    ImFont *display = nullptr; // 44 px
    // The pixel font of the logo, only the letters of "XScarlet", at the whole multiples
    // of its 13 px design size given by pixel_sizes: anything in between would blur it.
    std::array<ImFont *, 6> pixel{};
};
namespace color {
inline constexpr ImU32 scarlet = IM_COL32(236, 54, 48, 255);
inline constexpr ImU32 text = IM_COL32(236, 236, 242, 255);
inline constexpr ImU32 muted = IM_COL32(142, 142, 154, 255);
// `color` with its alpha multiplied by `alpha`
ImU32 fade(ImU32 color, float alpha);
} // namespace color

// Adds the fonts to the current context's atlas. Call once, before the first frame.
Fonts load_fonts();
void apply_style();

// The "XScarlet" wordmark in pixel art with its top left corner at `at`. It is drawn at the
// largest of pixel_sizes that is not above `size`; wordmark_size tells what that comes to.
ImVec2 wordmark_size(const Fonts &fonts, float size);
void wordmark(ImDrawList *list, const Fonts &fonts, ImVec2 at, float size, float alpha = 1);
// What is behind everything else between `min` and `max`: a dark gradient with a scarlet
// glow rising from the bottom and, unless `mark` is 0, a large faint wordmark on it.
void backdrop(const Fonts &fonts, ImVec2 min, ImVec2 max, float mark);
// `text` in `font`, centered on `x`, top at `y`
void centered_text(ImDrawList *list, ImFont *font, float x, float y, ImU32 color, const char *text);
} // namespace xs::ui
