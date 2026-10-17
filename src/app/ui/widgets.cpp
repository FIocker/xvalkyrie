#include "app/ui/widgets.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace xs::ui {
std::string fitted(const std::string &text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width)
        return text;
    auto cut = text;
    while (!cut.empty()) {
        unsigned char last; // a whole UTF-8 character at a time
        do {
            last = static_cast<unsigned char>(cut.back());
            cut.pop_back();
        } while (!cut.empty() && (last & 0xc0) == 0x80);
        if (ImGui::CalcTextSize((cut + "...").c_str()).x <= width)
            break;
    }
    return cut + "...";
}

float badge(ImDrawList *list, const Fonts &fonts, ImVec2 at, const Badge &badge) {
    auto *const font = fonts.small;
    const auto size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, badge.label);
    const float width = size.x + 14;
    list->AddRectFilled(at, {at.x + width, at.y + badge_height}, badge.fill, badge_height / 2);
    list->AddText(font, font->FontSize, {at.x + 7, at.y + (badge_height - size.y) / 2}, badge.ink,
                  badge.label);
    return width;
}

float key_hint(ImDrawList *list, const Fonts &fonts, ImVec2 at, const char *key,
               const char *action) {
    auto *const font = fonts.small;
    const auto cap = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, key);
    const auto text = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, action);
    const float cap_width = cap.x + 12, height = cap.y + 4;
    if (list) {
        list->AddRectFilled(at, {at.x + cap_width, at.y + height}, IM_COL32(255, 255, 255, 30), 4);
        list->AddRect(at, {at.x + cap_width, at.y + height}, IM_COL32(255, 255, 255, 46), 4);
        list->AddText(font, font->FontSize, {at.x + 6, at.y + 2}, color::text, key);
        list->AddText(font, font->FontSize, {at.x + cap_width + 6, at.y + 2}, color::muted, action);
    }
    return cap_width + 6 + text.x;
}

bool primary_button(const char *label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.925f, .21f, .19f, .88f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, .31f, .28f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.78f, .16f, .14f, 1));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

bool segmented(const char *id, std::span<const char *const> labels, int &chosen) {
    const auto &style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight(), rounding = style.FrameRounding;
    float widest = 0;
    for (const auto *label : labels)
        widest = std::max(widest, ImGui::CalcTextSize(label).x);
    const float cell = widest + 2 * style.FramePadding.x + 6;
    auto *const list = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetCursorScreenPos();
    list->AddRectFilled(origin,
                        {origin.x + cell * static_cast<float>(labels.size()), origin.y + height},
                        ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    bool changed = false;
    ImGui::PushID(id);
    for (int index = 0; index < static_cast<int>(labels.size()); ++index) {
        if (index)
            ImGui::SameLine(0, 0);
        const auto min = ImGui::GetCursorScreenPos();
        ImGui::PushID(index);
        const bool pressed = ImGui::InvisibleButton("segment", {cell, height});
        ImGui::PopID();
        const bool on = index == chosen;
        if (on)
            list->AddRectFilled({min.x + 2, min.y + 2}, {min.x + cell - 2, min.y + height - 2},
                                color::fade(color::scarlet, .9f), rounding - 1);
        else if (ImGui::IsItemHovered())
            list->AddRectFilled({min.x + 2, min.y + 2}, {min.x + cell - 2, min.y + height - 2},
                                IM_COL32(255, 255, 255, 22), rounding - 1);
        const auto *const label = labels[static_cast<std::size_t>(index)];
        const auto size = ImGui::CalcTextSize(label);
        list->AddText({min.x + (cell - size.x) / 2, min.y + (height - size.y) / 2},
                      on ? IM_COL32_WHITE : color::muted, label);
        if (pressed && !on) {
            chosen = index;
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

float animate(ImGuiID id, float target, float rate) {
    auto &value = *ImGui::GetStateStorage()->GetFloatRef(id, target);
    value += (target - value) * std::min(1.f, ImGui::GetIO().DeltaTime * rate);
    if (std::abs(target - value) < .004f)
        value = target;
    return value;
}

void section(const Fonts &fonts, const char *title) {
    ImGui::Dummy({0, 6});
    ImGui::PushFont(fonts.heading);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy({0, 2});
}
bool begin_settings(const char *id) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4, 9});
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH)) {
        ImGui::PopStyleVar();
        return false;
    }
    ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed);
    return true;
}
void setting(const Fonts &fonts, const char *title, const char *hint) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (!hint)
        ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    if (hint)
        note(fonts, hint);
    ImGui::TableNextColumn();
}
void end_settings() {
    ImGui::EndTable();
    ImGui::PopStyleVar();
}
void note(const Fonts &fonts, const char *text) {
    ImGui::PushFont(fonts.small);
    ImGui::PushStyleColor(ImGuiCol_Text, color::muted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
} // namespace xs::ui
