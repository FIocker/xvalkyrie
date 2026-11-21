#include "app/gui/file_dialog.hpp"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <nfd_glfw3.h>
#include <stdexcept>
#include <string>

namespace xv {
namespace {
std::runtime_error dialog_error() {
    const char *reason = NFD_GetError();
    return std::runtime_error(reason ? reason : "The native file dialog is unavailable");
}
struct Session {
    Session() {
        if (NFD_Init() != NFD_OKAY)
            throw dialog_error();
    }
    ~Session() { NFD_Quit(); }
};
} // namespace

std::future<std::filesystem::path> pick_title_file(GLFWwindow *owner) {
    nfdwindowhandle_t parent{};
    NFD_GetNativeWindowFromGLFWWindow(owner, &parent);
    return std::async(std::launch::async, [parent] {
        Session session;
        // The dialog adds its own "All files" entry. Packages are left out: they cannot be loaded.
        const nfdu8filteritem_t filters[] = {{"Title executables", "exe"}};
        nfdopendialogu8args_t args{};
        args.filterList = filters;
        args.filterCount = 1;
        args.parentWindow = parent;
        nfdu8char_t *selected = nullptr;
        const auto result = NFD_OpenDialogU8_With(&selected, &args);
        if (result == NFD_CANCEL)
            return std::filesystem::path{};
        if (result != NFD_OKAY)
            throw dialog_error();
        const std::string text = selected;
        NFD_FreePathU8(selected);
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    });
}
std::future<std::filesystem::path> pick_title_folder(GLFWwindow *owner) {
    nfdwindowhandle_t parent{};
    NFD_GetNativeWindowFromGLFWWindow(owner, &parent);
    return std::async(std::launch::async, [parent] {
        Session session;
        nfdpickfolderu8args_t args{};
        args.parentWindow = parent;
        nfdu8char_t *selected = nullptr;
        const auto result = NFD_PickFolderU8_With(&selected, &args);
        if (result == NFD_CANCEL)
            return std::filesystem::path{};
        if (result != NFD_OKAY)
            throw dialog_error();
        const std::string text = selected;
        NFD_FreePathU8(selected);
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    });
}
} // namespace xv
