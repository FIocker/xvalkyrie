#include "app/ui/settings.hpp"
#include "app/ui/widgets.hpp"
#include "app/version.hpp"
#include "backends/input/input.hpp"
#include <algorithm>
#include <array>
#include <exception>

namespace xs::ui {
namespace {
struct PageEntry {
    Settings::Page page;
    const char *name;
};
constexpr std::array pages{PageEntry{Settings::Page::input, "Input"},
                           PageEntry{Settings::Page::libraries, "Library"},
                           PageEntry{Settings::Page::about, "About"}};

std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
// an entry of the page list
bool page_entry(const char *name, bool shown) {
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0, .5f});
    const bool pressed = ImGui::Selectable((std::string("   ") + name).c_str(), shown, 0, {0, 34});
    ImGui::PopStyleVar();
    return pressed;
}
// a path that can be selected and copied
void path_field(const char *id, const std::filesystem::path &path) {
    auto text = utf8(path);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText(id, text.data(), text.size() + 1, ImGuiInputTextFlags_ReadOnly);
}

// The controls in the order of input::Control, as the groups a person looks for them in.
struct ControlGroup {
    const char *name;
    input::Control first, end;
};
constexpr std::array control_groups{
    ControlGroup{"Buttons", input::Control::a, input::Control::dpad_up},
    ControlGroup{"D-pad", input::Control::dpad_up, input::Control::left_shoulder},
    ControlGroup{"Bumpers, triggers and stick buttons", input::Control::left_shoulder,
                 input::Control::left_stick_up},
    ControlGroup{"Left stick", input::Control::left_stick_up, input::Control::right_stick_up},
    ControlGroup{"Right stick", input::Control::right_stick_up, input::Control::count},
};
} // namespace

void Settings::draw(const Fonts &fonts, const Host &host, Library library) {
    if (!input_shown())
        binding_.reset();
    if (!open_)
        return;
    const auto *const screen = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(
        {std::min(860.f, screen->WorkSize.x - 40), std::min(580.f, screen->WorkSize.y - 60)},
        ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(screen->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    ImGui::SetNextWindowSizeConstraints({560, 360}, {FLT_MAX, FLT_MAX});
    if (raise_)
        ImGui::SetNextWindowFocus();
    raise_ = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    const bool visible = ImGui::Begin("Settings", &open_, ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();
    if (visible) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10, 14});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, .28f));
        ImGui::BeginChild("Pages", {176, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
        for (const auto &entry : pages)
            if (page_entry(entry.name, entry.page == page_))
                page_ = entry.page;
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {26, 14});
        ImGui::BeginChild("Page", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
        switch (page_) {
        case Page::input:
            input(fonts, host);
            break;
        case Page::libraries:
            libraries(fonts, host, library);
            break;
        case Page::about:
            about(fonts);
            break;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    ImGui::End();
}

// Where the gamepad's readings come from and which key is which control. Changes apply
// at once.
void Settings::input(const Fonts &fonts, const Host &host) {
    auto settings = input::settings();
    const auto changed = [&] {
        input::configure(settings);
        input_notice_.clear();
        try {
            if (host.input_saved)
                host.input_saved();
        } catch (const std::exception &e) {
            input_notice_ = std::string("Not saved: ") + e.what();
        }
    };
    const bool direct = input::direct_capture_available();
    section(fonts, "Capture");
    if (begin_settings("capture")) {
        setting(fonts, "Method",
                !direct ? "Window: the keyboard, from this window's events. Direct capture is "
                          "not available on this host."
                : settings.capture == input::Capture::direct
                    ? "Direct: the gamepad (XInput) and the keyboard, read from the system."
                    : "Window: the keyboard, from this window's events.");
        static constexpr const char *methods[] = {"Window", "Direct"};
        int method = settings.capture == input::Capture::direct ? 1 : 0;
        ImGui::BeginDisabled(!direct);
        if (segmented("method", methods, method)) {
            settings.capture = method ? input::Capture::direct : input::Capture::window;
            changed();
        }
        ImGui::EndDisabled();
        setting(fonts, "Host gamepad", "With direct capture, also read the gamepad plugged in.");
        ImGui::BeginDisabled(settings.capture != input::Capture::direct);
        if (ImGui::Checkbox("##gamepad", &settings.host_gamepad))
            changed();
        ImGui::EndDisabled();
        end_settings();
    }

    section(fonts, "Keyboard");
    note(fonts, "Click a key to change it, then press the new one. Escape leaves it as it was.");
    if (binding_ && pressed_) {
        if (pressed_ != 0x1b)
            settings.keys[*binding_] = pressed_;
        binding_.reset();
        pressed_ = 0;
        changed();
    }
    constexpr float key_width = 132;
    for (const auto &group : control_groups) {
        ImGui::Dummy({0, 4});
        ImGui::PushFont(fonts.small);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::muted), "%s", group.name);
        ImGui::PopFont();
        if (!ImGui::BeginTable(group.name, 2, ImGuiTableFlags_SizingStretchSame))
            continue;
        for (auto index = static_cast<std::size_t>(group.first);
             index < static_cast<std::size_t>(group.end); ++index) {
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(index));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(input::control_name(static_cast<input::Control>(index)));
            const float clear = ImGui::GetFrameHeight();
            ImGui::SameLine(std::max(120.f, ImGui::GetContentRegionAvail().x - key_width - clear -
                                                ImGui::GetStyle().ItemSpacing.x - 12));
            const bool waiting = binding_ && *binding_ == index;
            const auto label =
                waiting ? std::string("Press a key...") : input::key_name(settings.keys[index]);
            if (waiting ? primary_button(label.c_str(), {key_width, 0})
                        : ImGui::Button(label.c_str(), {key_width, 0})) {
                binding_ = index;
                pressed_ = 0;
            }
            ImGui::SameLine(0, 4);
            ImGui::PushStyleColor(ImGuiCol_Button, 0);
            if (ImGui::Button("\xc3\x97", {clear, 0})) { // a multiplication sign as a cross
                settings.keys[index] = 0;
                binding_.reset();
                changed();
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Clear");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Dummy({0, 8});
    if (ImGui::Button("Restore default keys")) {
        settings.keys = input::default_settings().keys;
        binding_.reset();
        changed();
    }
    if (!input_notice_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", input_notice_.c_str());
    }
}

void Settings::libraries(const Fonts &fonts, const Host &host, Library library) {
    auto &settings = library.settings;
    section(fonts, "Game folders");
    note(fonts, "Folders with unpacked titles or XVD/XVC packages. Packages are listed but "
                "cannot be started yet.");
    ImGui::Dummy({0, 4});
    if (settings.directories.empty())
        ImGui::TextDisabled("No folders yet.");
    const auto &style = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Change...").x + ImGui::CalcTextSize("Remove").x +
                          4 * style.FramePadding.x + style.ItemSpacing.x;
    if (!settings.directories.empty() && begin_settings("folders")) {
        for (std::size_t index = 0; index < settings.directories.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const auto path = utf8(settings.directories[index]);
            ImGui::TextUnformatted(
                fitted(path, ImGui::GetContentRegionAvail().x - buttons - 24).c_str());
            ImGui::SetItemTooltip("%s", path.c_str());
            ImGui::TableNextColumn();
            if (ImGui::Button("Change...") && host.change_folder)
                host.change_folder(index);
            ImGui::SameLine();
            const bool remove = ImGui::Button("Remove");
            ImGui::PopID();
            if (remove) {
                settings.directories.erase(settings.directories.begin() +
                                           static_cast<std::ptrdiff_t>(index));
                if (host.libraries_saved)
                    host.libraries_saved(true);
                break;
            }
        }
        end_settings();
    }
    ImGui::Dummy({0, 6});
    if (primary_button("Add folder...") && host.add_folder)
        host.add_folder();
    ImGui::SameLine();
    if (ImGui::Button("Rescan") && host.rescan)
        host.rescan();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (library.scanning)
        ImGui::TextDisabled("Looking for titles...");
    else
        ImGui::TextDisabled("%zu %s found", library.titles,
                            library.titles == 1 ? "title" : "titles");
    if (!library.notice.empty())
        ImGui::TextWrapped("%s", library.notice.c_str());

    section(fonts, "View");
    if (begin_settings("view")) {
        setting(fonts, "Layout", "Cards with the logo of each title, or one title per line.");
        static constexpr const char *layouts[] = {"Grid", "List"};
        int layout = settings.grid ? 0 : 1;
        if (segmented("layout", layouts, layout)) {
            settings.grid = layout == 0;
            if (host.libraries_saved)
                host.libraries_saved(false);
        }
        end_settings();
    }
}

void Settings::about(const Fonts &fonts) {
    ImGui::Dummy({0, 6});
    const auto extent = wordmark_size(fonts, 52);
    wordmark(ImGui::GetWindowDrawList(), fonts, ImGui::GetCursorScreenPos(), 52);
    ImGui::Dummy(extent);
    ImGui::TextDisabled("%s", version);
    ImGui::Dummy({0, 6});
    ImGui::TextWrapped("Experimental Xbox One and Xbox Series X|S compatibility layer.");
    note(fonts, "Not affiliated with or endorsed by Microsoft. MIT license.");
    section(fonts, "Files");
    note(fonts, "Configuration");
    path_field("##configuration", config_file());
    ImGui::Dummy({0, 2});
    note(fonts, "Session logs, the files to attach when reporting a problem");
    path_field("##logs", config_directory() / "logs");
}
} // namespace xs::ui
