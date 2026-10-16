#include "frontend/desktop.hpp"
#include "app/launch.hpp"
#include "app/library.hpp"
#include "app/version.hpp"
#include "frontend/file_dialog.hpp"
#include "frontend/logo.hpp"
#include "input/input.hpp"
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <future>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace xs {
namespace {
struct Glfw {
    Glfw() {
        if (!glfwInit())
            throw std::runtime_error("GLFW initialization failed; check the display connection");
    }
    ~Glfw() { glfwTerminate(); }
};
struct Interface {
    explicit Interface(GLFWwindow *window) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
            ImGui::DestroyContext();
            throw std::runtime_error("ImGui window backend initialization failed");
        }
        if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            throw std::runtime_error("ImGui renderer initialization failed");
        }
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontDefault();
        // the built-in font is a 13px pixel font, so whole multiples stay sharp
        ImFontConfig large;
        large.SizePixels = 26;
        heading = io.Fonts->AddFontDefault(&large);
        large.SizePixels = 78;
        logo = io.Fonts->AddFontDefault(&large);
        ImGui::GetStyle().WindowRounding = 0;
    }
    ~Interface() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    ImFont *heading = nullptr;
    ImFont *logo = nullptr;
};
struct Guest {
    std::thread thread;
    std::atomic<bool> done = false;
    std::uint32_t code = 0;
    std::exception_ptr error;
};
std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

// GLFW key -> Windows virtual-key code, which is what input bindings are in. 0 if none.
int virtual_key(int key) {
    if ((key >= GLFW_KEY_0 && key <= GLFW_KEY_9) || (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) ||
        key == GLFW_KEY_SPACE)
        return key;
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12)
        return 0x70 + key - GLFW_KEY_F1;
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9)
        return 0x60 + key - GLFW_KEY_KP_0;
    switch (key) {
    case GLFW_KEY_ENTER:
        return 0x0d;
    case GLFW_KEY_TAB:
        return 0x09;
    case GLFW_KEY_BACKSPACE:
        return 0x08;
    case GLFW_KEY_ESCAPE:
        return 0x1b;
    case GLFW_KEY_LEFT:
        return 0x25;
    case GLFW_KEY_UP:
        return 0x26;
    case GLFW_KEY_RIGHT:
        return 0x27;
    case GLFW_KEY_DOWN:
        return 0x28;
    case GLFW_KEY_INSERT:
        return 0x2d;
    case GLFW_KEY_DELETE:
        return 0x2e;
    case GLFW_KEY_PAGE_UP:
        return 0x21;
    case GLFW_KEY_PAGE_DOWN:
        return 0x22;
    case GLFW_KEY_HOME:
        return 0x24;
    case GLFW_KEY_END:
        return 0x23;
    case GLFW_KEY_LEFT_SHIFT:
    case GLFW_KEY_RIGHT_SHIFT:
        return 0x10;
    case GLFW_KEY_LEFT_CONTROL:
    case GLFW_KEY_RIGHT_CONTROL:
        return 0x11;
    case GLFW_KEY_LEFT_ALT:
    case GLFW_KEY_RIGHT_ALT:
        return 0x12;
    case GLFW_KEY_SEMICOLON:
        return 0xba;
    case GLFW_KEY_EQUAL:
        return 0xbb;
    case GLFW_KEY_COMMA:
        return 0xbc;
    case GLFW_KEY_MINUS:
        return 0xbd;
    case GLFW_KEY_PERIOD:
        return 0xbe;
    case GLFW_KEY_SLASH:
        return 0xbf;
    case GLFW_KEY_GRAVE_ACCENT:
        return 0xc0;
    case GLFW_KEY_LEFT_BRACKET:
        return 0xdb;
    case GLFW_KEY_BACKSLASH:
        return 0xdc;
    case GLFW_KEY_RIGHT_BRACKET:
        return 0xdd;
    case GLFW_KEY_APOSTROPHE:
        return 0xde;
    default:
        return 0;
    }
}

void *native_window([[maybe_unused]] GLFWwindow *window) {
#ifdef _WIN32
    return glfwGetWin32Window(window);
#else
    return nullptr;
#endif
}

class Desktop {
  public:
    Desktop(GLFWwindow *window, const Interface &ui, const DesktopHooks &hooks)
        : window_(window), ui_(ui), hooks_(hooks), libraries_(hooks.libraries) {
        glfwSetWindowUserPointer(window, this);
        glfwSetDropCallback(window, [](GLFWwindow *target, int count, const char **paths) {
            if (count > 0) {
                const std::string text = paths[0];
                static_cast<Desktop *>(glfwGetWindowUserPointer(target))->dropped_ =
                    std::filesystem::path(std::u8string(text.begin(), text.end()));
            }
        });
        refresh_library();
    }
    ~Desktop() {
        // A guest that is still running cannot be stopped; it ends with the process.
        if (guest_)
            guest_->thread.detach();
    }
    int exit_code() const { return exit_code_; }
    // A key of the main window, before the interface sees it. This is the "window"
    // capture method: the keys go to the title unless the options are open, where the
    // next key pressed becomes a binding instead.
    void key(int glfw_key, bool pressed) {
        const int code = virtual_key(glfw_key);
        if (!code)
            return;
        if (binding_ && pressed) {
            pressed_ = code;
            return;
        }
        if (!input_open_ || !pressed)
            input::window_key(code, pressed);
    }
    // something on screen moves without input: the loading bar
    bool animating() const {
        return loading_.valid() || discovery_.valid() || (guest_ && !showing());
    }
    // the native dialog is modal: the window stays open until it is dismissed
    bool dialog_open() const { return dialog_.valid(); }

    // Reading and checking a title takes a while, so it happens off the window thread;
    // start() picks the result up.
    void open(const std::filesystem::path &path) {
        if (busy())
            return;
        notice_.clear();
        outcome_.clear();
        title_.reset();
        name_ = utf8(path.filename());
        if (const auto it = std::find_if(titles_.begin(), titles_.end(),
                                         [&](const auto &title) { return title.path == path; });
            it != titles_.end())
            name_ = it->name;
        glfwSetWindowTitle(window_, ("XScarlet - " + name_).c_str());
        path_ = path;
        loading_ = std::async(std::launch::async, hooks_.load, path);
    }
    void start() {
        const auto path = path_;
        try {
            title_ = loading_.get();
        } catch (const std::exception &e) {
            notice_ = std::string("Could not load the title: ") + e.what();
            exit_code_ = 1;
            return;
        }
        exit_code_ = title_->status == LaunchStatus::runnable ? 0 : 1;
        if (title_->status != LaunchStatus::runnable)
            return;
        // shared so that a guest outliving the window still has somewhere to report to
        guest_ = std::make_shared<Guest>();
        guest_->thread = std::thread([guest = guest_, run = hooks_.run, path] {
            try {
                guest->code = run(path);
            } catch (...) {
                guest->error = std::current_exception();
            }
            guest->done = true;
        });
    }

    void frame() {
        poll();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal))
            pick();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal))
            glfwSetWindowShouldClose(window_, true);
        menu();
        input_options();
        library_options();
        status_bar();
        content();
        about();
    }

  private:
    bool busy() const { return guest_ || dialog_.valid() || loading_.valid(); }
    enum class Dialog { file, folder, add_library, replace_library };
    // the title has frames on screen, in the area left for it
    bool showing() const { return guest_ && hooks_.showing && hooks_.showing(); }
    void pick() {
        if (!busy()) {
            dialog_kind_ = Dialog::file;
            dialog_ = pick_title_file(window_);
        }
    }
    void pick_folder(Dialog kind, std::size_t index = 0) {
        if (!busy()) {
            dialog_kind_ = kind;
            replace_index_ = index;
            dialog_ = pick_title_folder(window_);
        }
    }
    void refresh_library() {
        if (discovery_.valid()) {
            refresh_pending_ = true;
            return;
        }
        discovery_ = std::async(std::launch::async, discover_library, libraries_.directories);
    }
    void save_libraries() {
        try {
            if (hooks_.libraries_changed)
                hooks_.libraries_changed(libraries_);
            library_notice_.clear();
        } catch (const std::exception &e) {
            library_notice_ = std::string("Not saved: ") + e.what();
        }
        refresh_library();
    }
    void close_title() {
        title_.reset();
        notice_.clear();
        exit_code_ = 0;
        glfwSetWindowTitle(window_, "XScarlet");
    }
    void poll() {
        if (dialog_.valid() &&
            dialog_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                const auto path = dialog_.get();
                if (!path.empty()) {
                    if (dialog_kind_ == Dialog::file || dialog_kind_ == Dialog::folder) {
                        open(path);
                    } else if (std::filesystem::is_directory(path)) {
                        const auto normalized = std::filesystem::weakly_canonical(path);
                        if (dialog_kind_ == Dialog::replace_library &&
                            replace_index_ < libraries_.directories.size())
                            libraries_.directories[replace_index_] = normalized;
                        else if (dialog_kind_ == Dialog::add_library &&
                                 std::find(libraries_.directories.begin(),
                                           libraries_.directories.end(),
                                           normalized) == libraries_.directories.end())
                            libraries_.directories.push_back(normalized);
                        save_libraries();
                    }
                }
            } catch (const std::exception &e) {
                notice_ = std::string("Folder dialog failed: ") + e.what();
            }
        }
        if (discovery_.valid() &&
            discovery_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                titles_ = discovery_.get();
            } catch (const std::exception &e) {
                library_notice_ = std::string("Scan failed: ") + e.what();
            }
            if (refresh_pending_) {
                refresh_pending_ = false;
                refresh_library();
            }
        }
        if (loading_.valid() &&
            loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            start();
        if (guest_ && guest_->done) {
            guest_->thread.join();
            try {
                if (guest_->error)
                    std::rethrow_exception(guest_->error);
                char code[16];
                std::snprintf(code, sizeof code, "0x%x", guest_->code);
                outcome_ = std::string("Exited with code ") + code;
                exit_code_ = static_cast<int>(guest_->code);
            } catch (const std::exception &e) {
                outcome_ = std::string("Stopped: ") + e.what();
                exit_code_ = 1;
            }
            guest_.reset();
        }
        if (!dropped_.empty() && !busy())
            open(dropped_);
        dropped_.clear();
    }
    void menu() {
        if (!ImGui::BeginMainMenuBar())
            return;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open File...", "Ctrl+O", false, !busy()))
                pick();
            if (ImGui::MenuItem("Open Folder...", nullptr, false, !busy()))
                pick_folder(Dialog::folder);
            if (ImGui::MenuItem("Close", nullptr, false, title_ && !busy()))
                close_title();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Ctrl+Q"))
                glfwSetWindowShouldClose(window_, true);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Options")) {
            if (ImGui::MenuItem("Input..."))
                input_open_ = true;
            if (ImGui::MenuItem("Game Libraries..."))
                library_open_ = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About XScarlet"))
                about_requested_ = true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    const char *state() const {
        if (!notice_.empty())
            return notice_.c_str();
        if (loading_.valid())
            return "Loading";
        if (!title_)
            return "No title loaded";
        if (guest_)
            return showing() ? "Running" : "Starting";
        if (!outcome_.empty())
            return outcome_.c_str();
        return "Not started";
    }
    void status_bar() {
        const auto *screen = ImGui::GetMainViewport();
        const float height = ImGui::GetFrameHeight();
        ImGui::SetNextWindowPos(
            {screen->WorkPos.x, screen->WorkPos.y + screen->WorkSize.y - height});
        ImGui::SetNextWindowSize({screen->WorkSize.x, height});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().FramePadding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
        ImGui::Begin("Status", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (title_)
            ImGui::Text("%s  |  %s", name_.c_str(), state());
        else
            ImGui::TextUnformatted(state());
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(version).x -
                        ImGui::GetStyle().FramePadding.x);
        ImGui::TextDisabled("%s", version);
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
    static void centered(const char *text) {
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(text).x) / 2);
        ImGui::TextDisabled("%s", text);
    }
    // What stays on screen from the moment a title is opened until its first frame.
    void loading_screen(const char *step) {
        const float line = ImGui::GetTextLineHeightWithSpacing();
        ImGui::SetCursorPosY((ImGui::GetWindowHeight() - ui_.logo->FontSize - 5 * line) / 2);
        ImGui::PushFont(ui_.logo);
        centered("XScarlet");
        ImGui::PopFont();
        ImGui::Spacing();
        centered(name_.c_str());
        ImGui::Spacing();
        const float width = std::min(360.f, ImGui::GetWindowWidth() - 64);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) / 2);
        // how long a title takes to start is not known, so the bar only shows activity
        ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()), {width, 6}, "");
        ImGui::Spacing();
        centered(step);
    }
    void content() {
        const auto *screen = ImGui::GetMainViewport();
        if (hooks_.area) {
            // the largest 16:9 rectangle between the menu and the status bar, centered
            const auto scale = ImGui::GetIO().DisplayFramebufferScale;
            const float room_x = screen->WorkSize.x * scale.x,
                        room_y = (screen->WorkSize.y - ImGui::GetFrameHeight()) * scale.y;
            const float width = std::min(room_x, room_y * 16 / 9), height = width * 9 / 16;
            hooks_.area(static_cast<int>(screen->WorkPos.x * scale.x + (room_x - width) / 2),
                        static_cast<int>(screen->WorkPos.y * scale.y + (room_y - height) / 2),
                        static_cast<int>(width), static_cast<int>(height));
        }
        if (showing())
            return; // the title draws there
        ImGui::SetNextWindowPos(screen->WorkPos);
        ImGui::SetNextWindowSize(
            {screen->WorkSize.x, screen->WorkSize.y - ImGui::GetFrameHeight()});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 12});
        ImGui::Begin("Content", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (loading_.valid()) {
            loading_screen("Reading the title");
        } else if (guest_) {
            loading_screen("Starting the title");
        } else if (title_) {
            if (ImGui::SmallButton("Back to library"))
                close_title();
            ImGui::Separator();
            ImGui::PushFont(ui_.heading);
            ImGui::TextUnformatted(name_.c_str());
            ImGui::PopFont();
            ImGui::TextDisabled("%s", title_->file.c_str());
            ImGui::Separator();
            ImGui::TextUnformatted(state());
            ImGui::Spacing();
            ImGui::TextWrapped("%s", title_->detail.c_str());
        } else {
            library_content();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
    void library_content() {
        // The library sits on top of the landing screen; keep the original
        // pixel-font logo visible in the space beneath the translucent panel.
        const float height = ImGui::GetContentRegionAvail().y;
        const float panel_height = titles_.empty() && !discovery_.valid()
                                       ? std::min(170.f, height * .55f)
                                       : std::max(130.f, height - ui_.logo->FontSize - 48.f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, .85f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.38f, .32f, .32f, .55f));
        ImGui::BeginChild("LibraryOverlay", {0, panel_height}, ImGuiChildFlags_Borders);
        ImGui::TextDisabled("LIBRARY  /  %zu TITLES", titles_.size());
        ImGui::Separator();
        if (discovery_.valid())
            ImGui::TextDisabled("Discovering games...");
        if (!library_notice_.empty())
            ImGui::TextWrapped("%s", library_notice_.c_str());
        if (titles_.empty() && !discovery_.valid()) {
            ImGui::Spacing();
            ImGui::TextWrapped("No titles found. Add a folder in Options > Game Libraries, "
                               "or open an unpacked title with File > Open Folder.");
            ImGui::Spacing();
            ImGui::TextDisabled("File > Open File... (Ctrl+O), or drop a title here");
        } else {
            const float tile = 178.f;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const int columns =
                libraries_.grid
                    ? std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) /
                                                   (tile + gap)))
                    : 1;
            if (ImGui::BeginChild("Games", {0, 0}, ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoBackground)) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>((titles_.size() + columns - 1) / columns),
                              (libraries_.grid ? 228.f : 44.f) + gap);
                while (clipper.Step())
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                        for (int column = 0; column < columns; ++column) {
                            const auto i = static_cast<std::size_t>(row * columns + column);
                            if (i >= titles_.size())
                                break;
                            const auto &title = titles_[i];
                            ImGui::PushID(static_cast<int>(i));
                            const char *platform = title.target == Target::durango ? "Xbox One"
                                                   : title.target == Target::scarlett
                                                       ? "Series X|S"
                                                       : "Unknown target";
                            if (libraries_.grid) {
                                if (column)
                                    ImGui::SameLine();
                                ImGui::BeginGroup();
                                ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                                      ImVec4(.09f, .09f, .10f, .9f));
                                ImGui::BeginChild("tile", {tile, 228}, ImGuiChildFlags_Borders);
                                const auto &image =
                                    title.logo.empty() ? empty_logo_ : logos_.get(title.logo);
                                if (image.texture) {
                                    const auto scale =
                                        std::min(152.f / image.width, 130.f / image.height);
                                    ImGui::Image(static_cast<ImTextureID>(image.texture),
                                                 {image.width * scale, image.height * scale});
                                } else {
                                    ImGui::Dummy({0, 130});
                                }
                                ImGui::TextWrapped("%s", title.name.c_str());
                                ImGui::TextDisabled("%s%s", platform,
                                                    title.package ? " - package" : "");
                                ImGui::EndChild();
                                ImGui::PopStyleColor();
                                if (ImGui::IsItemClicked() && !busy())
                                    open(title.path);
                                ImGui::EndGroup();
                            } else {
                                const auto &image =
                                    title.logo.empty() ? empty_logo_ : logos_.get(title.logo);
                                if (image.texture) {
                                    const auto scale =
                                        std::min(44.f / image.width, 44.f / image.height);
                                    ImGui::Image(static_cast<ImTextureID>(image.texture),
                                                 {image.width * scale, image.height * scale});
                                } else {
                                    ImGui::Dummy({44, 44});
                                }
                                if (ImGui::IsItemClicked() && !busy())
                                    open(title.path);
                                ImGui::SameLine();
                                const auto label =
                                    title.name + "  |  " + platform +
                                    (title.package ? "  |  Package (unsupported)" : "") + "  |  " +
                                    title.version;
                                if (ImGui::Selectable(label.c_str(), false, 0, {0, 44}) && !busy())
                                    open(title.path);
                            }
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(
                                    "%s\n%s\n%s", title.name.c_str(), utf8(title.path).c_str(),
                                    title.executable.empty() ? "Package cannot be mounted"
                                                             : utf8(title.executable).c_str());
                            ImGui::PopID();
                        }
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        const float remaining = ImGui::GetContentRegionAvail().y;
        ImGui::SetCursorPosY(
            ImGui::GetCursorPosY() +
            std::max(0.f,
                     (remaining - ui_.logo->FontSize - ImGui::GetTextLineHeightWithSpacing()) / 2));
        ImGui::PushFont(ui_.logo);
        centered("XScarlet");
        ImGui::PopFont();
        centered(version);
    }
    void library_options() {
        if (!library_open_)
            return;
        ImGui::SetNextWindowSize({620, 360}, ImGuiCond_Appearing);
        if (!ImGui::Begin("Game Libraries", &library_open_)) {
            ImGui::End();
            return;
        }
        ImGui::TextWrapped("Choose folders containing unpacked games or XVD/XVC packages. "
                           "Packages are listed but cannot be launched yet.");
        if (ImGui::Button("Add folder...", {120, 0}))
            pick_folder(Dialog::add_library);
        ImGui::SameLine();
        if (ImGui::Button("Rescan"))
            refresh_library();
        int layout = libraries_.grid ? 0 : 1;
        if (ImGui::Combo("Library view", &layout, "Grid\0List\0")) {
            libraries_.grid = layout == 0;
            save_libraries();
        }
        ImGui::Separator();
        for (std::size_t i = 0; i < libraries_.directories.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextWrapped("%s", utf8(libraries_.directories[i]).c_str());
            if (ImGui::SmallButton("Change..."))
                pick_folder(Dialog::replace_library, i);
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                libraries_.directories.erase(libraries_.directories.begin() +
                                             static_cast<std::ptrdiff_t>(i));
                save_libraries();
                ImGui::PopID();
                break;
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::End();
    }
    void changed(const input::Settings &settings) {
        input::configure(settings);
        input_notice_.clear();
        try {
            if (hooks_.input_changed)
                hooks_.input_changed();
        } catch (const std::exception &e) {
            input_notice_ = std::string("Not saved: ") + e.what();
        }
    }
    // Options > Input: where the gamepad's readings come from and which key is which
    // control. Changes apply at once.
    void input_options() {
        if (!input_open_) {
            binding_.reset();
            return;
        }
        ImGui::SetNextWindowSize({460, 0}, ImGuiCond_Appearing);
        const auto center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {.5f, .5f});
        if (!ImGui::Begin("Input", &input_open_, ImGuiWindowFlags_NoCollapse)) {
            ImGui::End();
            return;
        }
        auto settings = input::settings();
        const bool direct = input::direct_capture_available();
        ImGui::TextUnformatted("Capture method");
        if (ImGui::RadioButton("Window (software)", settings.capture == input::Capture::window)) {
            settings.capture = input::Capture::window;
            changed(settings);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("keyboard, from this window's events");
        ImGui::BeginDisabled(!direct);
        if (ImGui::RadioButton("Direct (system)", settings.capture == input::Capture::direct)) {
            settings.capture = input::Capture::direct;
            changed(settings);
        }
        ImGui::SameLine();
        ImGui::TextDisabled(direct ? "gamepad (XInput) and keyboard, read from the system"
                                   : "not available on this host");
        ImGui::BeginDisabled(settings.capture != input::Capture::direct);
        if (ImGui::Checkbox("Use the host's gamepad", &settings.host_gamepad))
            changed(settings);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::TextUnformatted("Keyboard");
        if (binding_ && pressed_) {
            if (pressed_ != 0x1b) // Escape leaves the binding as it was
                settings.keys[*binding_] = pressed_;
            binding_.reset();
            pressed_ = 0;
            changed(settings);
        }
        if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_SizingStretchProp,
                              {0, 14 * ImGui::GetFrameHeightWithSpacing()})) {
            for (std::size_t index = 0; index < input::control_count; ++index) {
                ImGui::PushID(static_cast<int>(index));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(input::control_name(static_cast<input::Control>(index)));
                ImGui::TableNextColumn();
                const bool waiting = binding_ && *binding_ == index;
                const auto label =
                    waiting ? std::string("Press a key...") : input::key_name(settings.keys[index]);
                if (ImGui::Button(label.c_str(), {140, 0})) {
                    binding_ = index;
                    pressed_ = 0;
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Clear")) {
                    settings.keys[index] = 0;
                    binding_.reset();
                    changed(settings);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Restore defaults")) {
            const auto defaults = input::default_settings();
            settings.keys = defaults.keys;
            binding_.reset();
            changed(settings);
        }
        if (!input_notice_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", input_notice_.c_str());
        }
        ImGui::End();
    }
    void about() {
        if (about_requested_)
            ImGui::OpenPopup("About XScarlet");
        about_requested_ = false;
        const auto center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {.5f, .5f});
        if (!ImGui::BeginPopupModal("About XScarlet", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::Text("XScarlet %s", version);
        ImGui::TextUnformatted("Experimental Xbox One and Xbox Series X|S compatibility layer.");
        ImGui::TextUnformatted("Not affiliated with or endorsed by Microsoft. MIT license.");
        ImGui::Spacing();
        if (ImGui::Button("OK", {96, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    GLFWwindow *window_;
    const Interface &ui_;
    const DesktopHooks &hooks_;
    std::optional<LaunchResult> title_;
    std::string name_;    // file name of title_
    std::string outcome_; // how the last run of title_ ended
    std::string notice_;  // frontend problem unrelated to title_
    std::shared_ptr<Guest> guest_;
    std::future<LaunchResult> loading_;
    std::filesystem::path path_; // what loading_ is reading
    std::future<std::filesystem::path> dialog_;
    Dialog dialog_kind_ = Dialog::file;
    std::size_t replace_index_ = 0;
    std::filesystem::path dropped_;
    LibrarySettings libraries_;
    std::vector<LibraryTitle> titles_;
    std::future<std::vector<LibraryTitle>> discovery_;
    bool refresh_pending_ = false;
    bool library_open_ = false;
    std::string library_notice_;
    Logos logos_;
    LogoImage empty_logo_;
    bool about_requested_ = false;
    bool input_open_ = false;
    std::optional<std::size_t> binding_; // control waiting for a key
    int pressed_ = 0;                    // the key it got
    std::string input_notice_;
    int exit_code_ = 0;
};
} // namespace

int run_desktop(const DesktopHooks &hooks, const std::filesystem::path &initial) {
    Glfw glfw;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window(
        glfwCreateWindow(960, 600, "XScarlet", nullptr, nullptr), glfwDestroyWindow);
    if (!window) {
        const char *message = nullptr;
        glfwGetError(&message);
        throw std::runtime_error(message ? message : "OpenGL 3.3 window creation failed");
    }
    glfwSetWindowSizeLimits(window.get(), 480, 320, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window.get());
    if (!gladLoadGL(glfwGetProcAddress))
        throw std::runtime_error("OpenGL function loading failed");
    glfwSwapInterval(1);
    // Set before the interface installs its own callbacks, which then call these first.
    glfwSetKeyCallback(window.get(), [](GLFWwindow *target, int key, int, int action, int) {
        if (auto *const desktop = static_cast<Desktop *>(glfwGetWindowUserPointer(target));
            desktop && action != GLFW_REPEAT)
            desktop->key(key, action == GLFW_PRESS);
    });
    glfwSetWindowFocusCallback(window.get(), [](GLFWwindow *, int focused) {
        if (!focused)
            input::window_focus_lost();
    });
    Interface ui(window.get());
    if (hooks.host)
        hooks.host(native_window(window.get()));
    Desktop desktop(window.get(), ui, hooks);
    if (!initial.empty())
        desktop.open(initial);
    for (;;) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window.get())) {
            if (!desktop.dialog_open())
                break;
            glfwSetWindowShouldClose(window.get(), false);
        }
        int w, h;
        glfwGetFramebufferSize(window.get(), &w, &h);
        if (w <= 0 || h <= 0) {
            glfwWaitEventsTimeout(.1);
            continue;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        desktop.frame();
        ImGui::Render();
        glViewport(0, 0, w, h);
        glClearColor(.09f, .09f, .1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window.get());
        // redraw every vblank while in use so menus respond at once, otherwise idle
        if (!desktop.animating() && !glfwGetWindowAttrib(window.get(), GLFW_FOCUSED) &&
            !glfwGetWindowAttrib(window.get(), GLFW_HOVERED))
            glfwWaitEventsTimeout(.1);
    }
    return desktop.exit_code();
}
} // namespace xs
