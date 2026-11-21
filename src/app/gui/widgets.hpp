#pragma once
#include "app/gui/theme.hpp"
#include <span>
#include <string>

// The pieces the screens and the settings are put together from. Everything here is
// stateless apart from animate(), which keeps its value in the current ImGui window.
namespace xv::ui {
// `text` cut short with "..." so that it fits in `width` in the current font
std::string fitted(const std::string &text, float width);

// A small pill with a word in it.
struct Badge {
    const char *label;
    ImU32 fill, ink;
};
namespace badges {
inline constexpr Badge xbox_one{"XBOX ONE", IM_COL32(16, 124, 16, 70),
                                IM_COL32(134, 226, 134, 255)};
inline constexpr Badge series{"SERIES X|S", IM_COL32(236, 54, 48, 60),
                              IM_COL32(255, 150, 140, 255)};
inline constexpr Badge unknown{"UNKNOWN", IM_COL32(255, 255, 255, 22), color::muted};
inline constexpr Badge package{"PACKAGE", IM_COL32(255, 255, 255, 22), color::muted};
inline constexpr Badge cannot_start{"CANNOT START", IM_COL32(236, 54, 48, 60),
                                    IM_COL32(255, 150, 140, 255)};
} // namespace badges
inline constexpr float badge_height = 19;
// draws it with its top left corner at `at`; returns its width
float badge(ImDrawList *list, const Fonts &fonts, ImVec2 at, const Badge &badge);

// "[Enter] Play": a key cap and what the key does. Returns the width; with a null list
// nothing is drawn, for measuring.
float key_hint(ImDrawList *list, const Fonts &fonts, ImVec2 at, const char *key,
               const char *action);

// the one button of a screen that is its main action
bool primary_button(const char *label, ImVec2 size = {0, 0});
// Choices of which exactly one is on, side by side. True when another one was picked,
// which `chosen` then is.
bool segmented(const char *id, std::span<const char *const> labels, int &chosen);

// That something is going on in the background: a short scarlet arc turning on a faint ring
// of `radius` around `center`.
void spinner(ImDrawList *list, ImVec2 center, float radius, float thickness = 2);

// A value that follows `target` a little more every frame. Kept per `id` in the current
// window; starts at the target.
float animate(ImGuiID id, float target, float rate = 16);

// ---- settings pages
// the heading of a group of settings
void section(const Fonts &fonts, const char *title);
// A group of settings, one per line: what it is on the left, its control on the right.
//     if (begin_settings("view")) {
//         setting(fonts, "Layout", "How the library is shown");
//         segmented(...);
//         end_settings();
//     }
bool begin_settings(const char *id);
void setting(const Fonts &fonts, const char *title, const char *hint = nullptr);
void end_settings();
// a line of secondary text, wrapped
void note(const Fonts &fonts, const char *text);
} // namespace xv::ui
