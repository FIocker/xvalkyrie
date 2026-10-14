#include "frontend/desktop.hpp"
#include "app/launch.hpp"
#include "app/version.hpp"
#include "frontend/file_dialog.hpp"
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
        : window_(window), ui_(ui), hooks_(hooks) {
        glfwSetWindowUserPointer(window, this);
        glfwSetDropCallback(window, [](GLFWwindow *target, int count, const char **paths) {
            if (count > 0) {
                const std::string text = paths[0];
                static_cast<Desktop *>(glfwGetWindowUserPointer(target))->dropped_ =
                    std::filesystem::path(std::u8string(text.begin(), text.end()));
            }
        });
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
    bool animating() const { return loading_.valid() || (guest_ && !showing()); }
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
        status_bar();
        content();
        about();
    }

  private:
    bool busy() const { return guest_ || dialog_.valid() || loading_.valid(); }
    // the title has frames on screen, in the area left for it
    bool showing() const { return guest_ && hooks_.showing && hooks_.showing(); }
    void pick() {
        if (!busy())
            dialog_ = pick_title_file(window_);
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
                if (const auto path = dialog_.get(); !path.empty())
                    open(path);
            } catch (const std::exception &e) {
                notice_ = std::string("Open File failed: ") + e.what();
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
            ImGui::PushFont(ui_.heading);
            ImGui::TextUnformatted(name_.c_str());
            ImGui::PopFont();
            ImGui::TextDisabled("%s", title_->file.c_str());
            ImGui::Separator();
            ImGui::TextUnformatted(state());
            ImGui::Spacing();
            ImGui::TextWrapped("%s", title_->detail.c_str());
        } else {
            const float line = ImGui::GetTextLineHeightWithSpacing();
            ImGui::SetCursorPosY((ImGui::GetWindowHeight() - ui_.logo->FontSize - 3 * line) / 2);
            ImGui::PushFont(ui_.logo);
            centered("XScarlet");
            ImGui::PopFont();
            centered(version);
            ImGui::Spacing();
            centered("File > Open File... (Ctrl+O), or drop a title executable or folder here");
        }
        ImGui::End();
        ImGui::PopStyleVar();
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
    std::filesystem::path dropped_;
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
