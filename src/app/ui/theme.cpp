#include "app/ui/theme.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>

// Roboto Medium, generated from the file by cmake/Embed.cmake
extern const unsigned char xv_font_roboto[];
extern const unsigned int xv_font_roboto_size;

namespace xv::ui {
ImU32 color::fade(ImU32 color, float alpha) {
    const auto old = static_cast<float>(color >> IM_COL32_A_SHIFT & 0xff);
    const auto value = static_cast<ImU32>(std::clamp(old * alpha, 0.f, 255.f));
    return (color & ~IM_COL32_A_MASK) | value << IM_COL32_A_SHIFT;
}

Fonts load_fonts() {
    auto &atlas = *ImGui::GetIO().Fonts;
    const auto add = [&](float size, const ImWchar *ranges = nullptr) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        return atlas.AddFontFromMemoryTTF(const_cast<unsigned char *>(xv_font_roboto),
                                          static_cast<int>(xv_font_roboto_size), size, &config,
                                          ranges);
    };
    // pairs of first and last character; the atlas keeps the pointer
    static const ImWchar letters[] = {'S', 'S', 'X', 'X', 'a', 'a', 'c', 'c', 'e',
                                      'e', 'l', 'l', 'r', 'r', 't', 't', 0};
    Fonts fonts;
    fonts.body = add(16);
    fonts.small = add(13);
    fonts.heading = add(24);
    fonts.display = add(44);
    for (std::size_t index = 0; index < pixel_sizes.size(); ++index) {
        ImFontConfig config; // Dear ImGui's built-in ProggyClean
        config.SizePixels = pixel_sizes[index];
        config.GlyphRanges = letters;
        fonts.pixel[index] = atlas.AddFontDefault(&config);
    }
    return fonts;
}

void apply_style() {
    auto &style = ImGui::GetStyle();
    style.WindowRounding = 10;
    style.ChildRounding = 10;
    style.PopupRounding = 8;
    style.FrameRounding = 6;
    style.GrabRounding = 6;
    style.ScrollbarRounding = 8;
    style.TabRounding = 6;
    style.WindowBorderSize = 1;
    style.FrameBorderSize = 0;
    style.WindowPadding = {16, 14};
    style.FramePadding = {10, 6};
    style.ItemSpacing = {8, 8};
    style.ItemInnerSpacing = {8, 6};
    style.CellPadding = {8, 5};
    style.ScrollbarSize = 12;
    style.WindowTitleAlign = {0, .5f};
    style.SeparatorTextBorderSize = 1;

    const auto rgba = [](int r, int g, int b, float a = 1) {
        return ImVec4(r / 255.f, g / 255.f, b / 255.f, a);
    };
    auto *const colors = style.Colors;
    colors[ImGuiCol_Text] = rgba(236, 236, 242);
    colors[ImGuiCol_TextDisabled] = rgba(142, 142, 154);
    colors[ImGuiCol_WindowBg] = rgba(20, 20, 25);
    colors[ImGuiCol_ChildBg] = rgba(0, 0, 0, 0);
    colors[ImGuiCol_PopupBg] = rgba(24, 24, 30);
    colors[ImGuiCol_Border] = rgba(255, 255, 255, .09f);
    colors[ImGuiCol_BorderShadow] = rgba(0, 0, 0, 0);
    colors[ImGuiCol_FrameBg] = rgba(255, 255, 255, .06f);
    colors[ImGuiCol_FrameBgHovered] = rgba(255, 255, 255, .10f);
    colors[ImGuiCol_FrameBgActive] = rgba(255, 255, 255, .14f);
    colors[ImGuiCol_TitleBg] = rgba(16, 16, 20);
    colors[ImGuiCol_TitleBgActive] = rgba(28, 28, 35);
    colors[ImGuiCol_TitleBgCollapsed] = rgba(16, 16, 20, .8f);
    colors[ImGuiCol_MenuBarBg] = rgba(13, 13, 16);
    colors[ImGuiCol_ScrollbarBg] = rgba(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab] = rgba(255, 255, 255, .14f);
    colors[ImGuiCol_ScrollbarGrabHovered] = rgba(255, 255, 255, .24f);
    colors[ImGuiCol_ScrollbarGrabActive] = rgba(236, 54, 48, .9f);
    colors[ImGuiCol_CheckMark] = rgba(236, 54, 48);
    colors[ImGuiCol_SliderGrab] = rgba(236, 54, 48, .85f);
    colors[ImGuiCol_SliderGrabActive] = rgba(255, 96, 86);
    colors[ImGuiCol_Button] = rgba(255, 255, 255, .08f);
    colors[ImGuiCol_ButtonHovered] = rgba(255, 255, 255, .15f);
    colors[ImGuiCol_ButtonActive] = rgba(236, 54, 48, .85f);
    colors[ImGuiCol_Header] = rgba(236, 54, 48, .28f);
    colors[ImGuiCol_HeaderHovered] = rgba(236, 54, 48, .40f);
    colors[ImGuiCol_HeaderActive] = rgba(236, 54, 48, .60f);
    colors[ImGuiCol_Separator] = rgba(255, 255, 255, .09f);
    colors[ImGuiCol_SeparatorHovered] = rgba(236, 54, 48, .6f);
    colors[ImGuiCol_SeparatorActive] = rgba(236, 54, 48);
    colors[ImGuiCol_ResizeGrip] = rgba(255, 255, 255, .08f);
    colors[ImGuiCol_ResizeGripHovered] = rgba(236, 54, 48, .6f);
    colors[ImGuiCol_ResizeGripActive] = rgba(236, 54, 48);
    colors[ImGuiCol_PlotHistogram] = rgba(236, 54, 48);
    colors[ImGuiCol_TextSelectedBg] = rgba(236, 54, 48, .40f);
    colors[ImGuiCol_NavCursor] = rgba(236, 54, 48, .9f);
    colors[ImGuiCol_ModalWindowDimBg] = rgba(0, 0, 0, .6f);
    colors[ImGuiCol_TableBorderLight] = rgba(255, 255, 255, .05f);
    colors[ImGuiCol_TableBorderStrong] = rgba(255, 255, 255, .09f);
}

namespace {
// the largest baked size of the logo font that is not above `size`
ImFont *wordmark_font(const Fonts &fonts, float size) {
    std::size_t index = 0;
    while (index + 1 < pixel_sizes.size() && pixel_sizes[index + 1] <= size)
        ++index;
    return fonts.pixel[index];
}
// A disc of `inner` at the center fading to nothing at `radius`.
void glow(ImDrawList *list, ImVec2 center, float radius, ImU32 inner) {
    constexpr int segments = 64;
    const auto uv = ImGui::GetFontTexUvWhitePixel();
    const auto first = list->_VtxCurrentIdx;
    list->PrimReserve(segments * 3, segments + 1);
    list->PrimWriteVtx(center, uv, inner);
    for (int i = 0; i < segments; ++i) {
        const float angle = 6.2831853f * static_cast<float>(i) / segments;
        list->PrimWriteVtx(
            {center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius}, uv,
            inner & ~IM_COL32_A_MASK);
    }
    for (int i = 0; i < segments; ++i) {
        list->PrimWriteIdx(static_cast<ImDrawIdx>(first));
        list->PrimWriteIdx(static_cast<ImDrawIdx>(first + 1 + i));
        list->PrimWriteIdx(static_cast<ImDrawIdx>(first + 1 + (i + 1) % segments));
    }
}
} // namespace

ImVec2 wordmark_size(const Fonts &fonts, float size) {
    auto *const font = wordmark_font(fonts, size);
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, "XValkyrie");
}
void wordmark(ImDrawList *list, const Fonts &fonts, ImVec2 at, float size, float alpha) {
    auto *const font = wordmark_font(fonts, size);
    // on whole pixels, or the font's own pixels straddle two of the screen's
    list->AddText(font, font->FontSize, {std::floor(at.x), std::floor(at.y)},
                  color::fade(color::text, alpha), "XValkyrie");
}
void centered_text(ImDrawList *list, ImFont *font, float x, float y, ImU32 color,
                   const char *text) {
    const auto size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, text);
    list->AddText(font, font->FontSize, {x - size.x / 2, y}, color, text);
}

void backdrop(const Fonts &fonts, ImVec2 min, ImVec2 max, float mark) {
    auto *const list = ImGui::GetBackgroundDrawList();
    const float width = max.x - min.x, height = max.y - min.y;
    list->PushClipRect(min, max);
    const ImU32 top = IM_COL32(17, 17, 22, 255), bottom = IM_COL32(9, 9, 12, 255);
    list->AddRectFilledMultiColor(min, max, top, top, bottom, bottom);
    glow(list, {min.x + width * .5f, max.y + height * .25f}, std::max(width * .62f, height),
         IM_COL32(236, 54, 48, 74));
    glow(list, {max.x, min.y}, width * .45f, IM_COL32(236, 54, 48, 22));
    if (mark > 0) {
        // resting on the bottom edge, where a library that fits leaves it in sight and a
        // longer one scrolls over it
        const float size = std::clamp(width * .2f, 78.f, 208.f);
        const auto extent = wordmark_size(fonts, size);
        wordmark(list, fonts, {min.x + (width - extent.x) / 2, max.y - extent.y * .86f}, size,
                 mark);
    }
    list->PopClipRect();
}
} // namespace xv::ui
