#include "frontend/desktop.hpp"
#include "app/launch.hpp"
#include "app/version.hpp"
#include "frontend/file_dialog.hpp"
#include <GLFW/glfw3.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
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
    // the native dialog is modal: the window stays open until it is dismissed
    bool dialog_open() const { return dialog_.valid(); }

    void open(const std::filesystem::path &path) {
        if (guest_)
            return;
        notice_.clear();
        outcome_.clear();
        title_ = hooks_.load(path);
        name_ = utf8(path.filename());
        glfwSetWindowTitle(window_, ("XScarlet - " + name_).c_str());
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
        status_bar();
        content();
        about();
    }

  private:
    bool busy() const { return guest_ || dialog_.valid(); }
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
        if (!title_)
            return "No title loaded";
        if (guest_)
            return "Running";
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
    void content() {
        const auto *screen = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(screen->WorkPos);
        ImGui::SetNextWindowSize(
            {screen->WorkSize.x, screen->WorkSize.y - ImGui::GetFrameHeight()});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 12});
        ImGui::Begin("Content", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (title_) {
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
            centered("File > Open File... (Ctrl+O), or drop a title executable here");
        }
        ImGui::End();
        ImGui::PopStyleVar();
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
        ImGui::TextUnformatted("Experimental Xbox Series X|S compatibility layer.");
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
    std::future<std::filesystem::path> dialog_;
    std::filesystem::path dropped_;
    bool about_requested_ = false;
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
    Interface ui(window.get());
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
        if (!glfwGetWindowAttrib(window.get(), GLFW_FOCUSED) &&
            !glfwGetWindowAttrib(window.get(), GLFW_HOVERED))
            glfwWaitEventsTimeout(.1);
    }
    return desktop.exit_code();
}
} // namespace xs
