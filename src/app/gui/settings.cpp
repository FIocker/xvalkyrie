#include "app/gui/settings.hpp"
#include "app/gui/widgets.hpp"
#include "app/version.hpp"
#include "backends/graphics/backend.hpp"
#include "backends/input/input.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>

namespace xv::ui {
namespace {
struct PageEntry {
    Settings::Page page;
    const char *name;
};
constexpr std::array pages{
    PageEntry{Settings::Page::input, "Input"}, PageEntry{Settings::Page::graphics, "Graphics"},
    PageEntry{Settings::Page::console, "Console"}, PageEntry{Settings::Page::libraries, "Library"},
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

void Settings::draw(const Fonts &fonts, const Host &host, Library library,
                    ConsoleSettings &profile) {
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
        case Page::graphics:
            graphics(fonts, host);
            break;
        case Page::console:
            console(fonts, host, profile);
            break;
        case Page::libraries:
            libraries(fonts, host, library);
            break;
        case Page::about:
            about(fonts, host);
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
    section(fonts, "Capture");
    if (begin_settings("capture")) {
        setting(fonts, "Method",
                settings.capture == input::Capture::direct
                    ? "Direct: the gamepad (XInput) and the keyboard, read from the system."
                    : "Window: the keyboard, from this window's events.");
        static constexpr const char *methods[] = {"Window", "Direct"};
        int method = settings.capture == input::Capture::direct ? 1 : 0;
        if (segmented("method", methods, method)) {
            settings.capture = method ? input::Capture::direct : input::Capture::window;
            changed();
        }
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

void Settings::graphics(const Fonts &fonts, const Host &host) {
    const auto backend = graphics::selected_backend();
    section(fonts, "Rendering");
    if (begin_settings("rendering")) {
        setting(fonts, "Backend",
                backend == graphics::Backend::vulkan
                    ? "Vulkan: Direct3D 11 translated to Vulkan, shipped with xvalkyrie."
                    : "Direct3D 11: the host's own driver.");
        static constexpr const char *backends[] = {"Direct3D 11", "Vulkan"};
        int chosen = backend == graphics::Backend::vulkan ? 1 : 0;
        if (segmented("backend", backends, chosen)) {
            const auto picked = chosen ? graphics::Backend::vulkan : graphics::Backend::d3d11;
            graphics_notice_.clear();
            if (!graphics::backend_renders_titles(picked))
                graphics_notice_ = std::string("Vulkan needs ") +
                                   std::string(graphics::vulkan_d3d11_module) + " and " +
                                   std::string(graphics::vulkan_dxgi_module) +
                                   " next to xvalkyrie.exe. Build with -DXV_DXVK=ON.";
            else {
                graphics::select_backend(picked);
                try {
                    if (host.graphics_saved)
                        host.graphics_saved();
                } catch (const std::exception &e) {
                    graphics_notice_ = std::string("Not saved: ") + e.what();
                }
            }
        }
        const auto fullscreen = graphics::fullscreen_mode();
        setting(fonts, "Full screen",
                fullscreen == graphics::Fullscreen::native
                    ? "Native: F11 gives the display to the title, where the host can."
                    : "Borderless: F11 makes the title's window as large as the display.");
        static constexpr const char *modes[] = {"Borderless", "Native"};
        int mode = fullscreen == graphics::Fullscreen::native ? 1 : 0;
        if (segmented("fullscreen", modes, mode)) {
            graphics::select_fullscreen_mode(mode ? graphics::Fullscreen::native
                                                  : graphics::Fullscreen::borderless);
            graphics_notice_.clear();
            try {
                if (host.graphics_saved)
                    host.graphics_saved();
            } catch (const std::exception &e) {
                graphics_notice_ = std::string("Not saved: ") + e.what();
            }
        }
        end_settings();
    }
    note(fonts, "Applies to the next title started.");
    if (!graphics_notice_.empty())
        ImGui::TextWrapped("%s", graphics_notice_.c_str());
}

void Settings::console(const Fonts &fonts, const Host &host, ConsoleSettings &settings) {
    if (!console_loaded_) {
        std::snprintf(gamertag_, sizeof gamertag_, "%s", settings.gamertag.c_str());
        std::snprintf(user_id_, sizeof user_id_, "%s", settings.xbox_user_id.c_str());
        console_loaded_ = true;
    }
    const auto changed = [&] {
        console_notice_.clear();
        try {
            if (host.console_saved)
                host.console_saved();
        } catch (const std::exception &e) {
            console_notice_ = std::string("Not saved: ") + e.what();
        }
    };
    constexpr float field = 220;
    section(fonts, "Profile");
    if (begin_settings("profile")) {
        setting(fonts, "Gamertag", "1 to 15 characters, shown by titles as the player's name.");
        ImGui::SetNextItemWidth(field);
        ImGui::InputText("##gamertag", gamertag_, sizeof gamertag_);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (valid_gamertag(gamertag_)) {
                settings.gamertag = gamertag_;
                changed();
            } else
                console_notice_ = "A gamertag has 1 to 15 characters.";
        }
        setting(fonts, "User ID",
                "The Xbox user ID (XUID). Some titles name their saves after it, so changing "
                "it hides those saves.");
        ImGui::SetNextItemWidth(field);
        ImGui::InputText("##xuid", user_id_, sizeof user_id_, ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (valid_xbox_user_id(user_id_)) {
                settings.xbox_user_id = user_id_;
                changed();
            } else
                console_notice_ = "A user ID has 1 to 20 digits.";
        }
        setting(fonts, "Gamerscore");
        ImGui::SetNextItemWidth(field);
        ImGui::InputScalar("##gamerscore", ImGuiDataType_U32, &settings.gamerscore);
        if (ImGui::IsItemDeactivatedAfterEdit())
            changed();
        setting(fonts, "Age group", "Titles with age ratings or chat limits look at it.");
        static constexpr const char *ages[] = {"Unknown", "Child", "Teen", "Adult"};
        int age = static_cast<int>(std::min(settings.age_group, 3u));
        if (segmented("age", ages, age)) {
            settings.age_group = static_cast<std::uint32_t>(age);
            changed();
        }
        end_settings();
    }
    section(fonts, "Accessories");
    if (begin_settings("accessories")) {
        setting(fonts, "Headset",
                "The user has an audio device of their own: this PC's communications output.");
        if (ImGui::Checkbox("##headset", &settings.headset))
            changed();
        end_settings();
    }
    section(fonts, "System keyboard");
    if (begin_settings("keyboard")) {
        setting(fonts, "When a title asks for text",
                settings.keyboard_dialog ? "A window on this PC asks for it."
                                         : "The title gets the text it suggested, unchanged.");
        static constexpr const char *modes[] = {"Ask", "Default text"};
        int mode = settings.keyboard_dialog ? 0 : 1;
        if (segmented("keyboard", modes, mode)) {
            settings.keyboard_dialog = mode == 0;
            changed();
        }
        end_settings();
    }
    note(fonts, "Applies to the next title started.");
    if (!console_notice_.empty())
        ImGui::TextWrapped("%s", console_notice_.c_str());
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

void Settings::about(const Fonts &fonts, const Host &host) {
    ImGui::Dummy({0, 6});
    const auto extent = wordmark_size(fonts, 52);
    wordmark(ImGui::GetWindowDrawList(), fonts, ImGui::GetCursorScreenPos(), 52);
    ImGui::Dummy(extent);
    ImGui::TextDisabled("%s", version);
    ImGui::Dummy({0, 6});
    ImGui::TextWrapped("Experimental Xbox One and Xbox Series X|S compatibility layer.");
    note(fonts, "Not affiliated with or endorsed by Microsoft. MIT license.");
    section(fonts, "Updates");
    const auto updates = host.updates();
    ImGui::BeginDisabled(updates.checking || updates.installing);
    if (ImGui::Button(updates.checking ? "Checking..." : "Check for Updates"))
        host.check_updates();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(updates.available.empty() || updates.installing || updates.blocked);
    const auto label = updates.installing          ? "Updating..."
                       : updates.available.empty() ? std::string("Update")
                                                   : "Update to " + updates.available;
    if (primary_button(label.c_str()))
        host.install_update();
    ImGui::EndDisabled();
    if (!updates.available.empty() && updates.blocked)
        note(fonts, "Close the running title to update.");
    else if (!updates.message.empty())
        note(fonts, updates.message.c_str());
    section(fonts, "Files");
    note(fonts, "Configuration");
    path_field("##configuration", config_file());
    ImGui::Dummy({0, 2});
    note(fonts, "Session logs, the files to attach when reporting a problem");
    path_field("##logs", config_directory() / "logs");
}
} // namespace xv::ui
