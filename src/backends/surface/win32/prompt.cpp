#include "backends/surface/surface.hpp"
#include <memory>
#include <thread>
#include <utility>
#include <windows.h>

// The system keyboard of a console, as a small window of the host's: a title, a line that
// says what is asked for, the text, OK and Cancel. It has a thread of its own, so the title
// goes on running while it is up, as it does on a console.
namespace xs::surface {
namespace {
constexpr wchar_t class_name[] = L"XScarletKeyboard";
constexpr int text_id = 100;
struct Prompt {
    TextRequest request;
    std::optional<std::wstring> result;
    HFONT font = nullptr, bold = nullptr;
    HWND text = nullptr;
};
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wide, LPARAM value) {
    auto *const prompt = reinterpret_cast<Prompt *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wide) == IDOK && prompt) {
            std::wstring entered(static_cast<std::size_t>(GetWindowTextLengthW(prompt->text)), 0);
            GetWindowTextW(prompt->text, entered.data(), static_cast<int>(entered.size() + 1));
            prompt->result = std::move(entered);
            DestroyWindow(window);
        } else if (LOWORD(wide) == IDCANCEL)
            DestroyWindow(window);
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wide), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    return DefWindowProcW(window, message, wide, value);
}
void run(Prompt &prompt) {
    WNDCLASSEXW description{sizeof description};
    description.lpfnWndProc = &procedure;
    description.hInstance = GetModuleHandleW(nullptr);
    description.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    description.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    description.lpszClassName = class_name;
    RegisterClassExW(&description);

    const auto owner = static_cast<HWND>(detail::title_owner());
    const auto dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    const auto scale = [&](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    NONCLIENTMETRICSW metrics{sizeof metrics};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0, dpi);
    prompt.font = CreateFontIndirectW(&metrics.lfMessageFont);
    auto heading = metrics.lfMessageFont;
    heading.lfWeight = FW_SEMIBOLD;
    heading.lfHeight = heading.lfHeight * 4 / 3;
    prompt.bold = CreateFontIndirectW(&heading);

    const int width = scale(460), height = scale(190), margin = scale(16), line = scale(24);
    RECT frame{0, 0, width, height};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    RECT area{};
    if (owner)
        GetWindowRect(owner, &area);
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    const int outer_width = frame.right - frame.left, outer_height = frame.bottom - frame.top;
    const auto window =
        CreateWindowExW(WS_EX_DLGMODALFRAME | (owner ? 0 : WS_EX_TOPMOST), class_name,
                        prompt.request.title.empty() ? L"Keyboard" : prompt.request.title.c_str(),
                        style, area.left + (area.right - area.left - outer_width) / 2,
                        area.top + (area.bottom - area.top - outer_height) / 2, outer_width,
                        outer_height, owner, nullptr, description.hInstance, nullptr);
    if (!window)
        return;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&prompt));
    const auto child = [&](const wchar_t *kind, const wchar_t *text, DWORD flags, int x, int y,
                           int w, int h, int id, HFONT font) {
        const auto control = CreateWindowExW(
            0, kind, text, WS_CHILD | WS_VISIBLE | flags, x, y, w, h, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), description.hInstance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    };
    const int inner = width - 2 * margin, button = scale(92);
    child(L"STATIC", prompt.request.title.c_str(), SS_LEFT | SS_ENDELLIPSIS, margin, margin, inner,
          line + scale(4), 0, prompt.bold);
    child(L"STATIC", prompt.request.description.c_str(), SS_LEFT | SS_ENDELLIPSIS, margin,
          margin + line + scale(8), inner, line, 0, prompt.font);
    prompt.text = child(
        L"EDIT", prompt.request.text.c_str(),
        WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | (prompt.request.password ? ES_PASSWORD : 0) |
            (prompt.request.digits ? ES_NUMBER : 0),
        margin, margin + 2 * line + scale(16), inner, line + scale(4), text_id, prompt.font);
    if (prompt.request.max_length)
        SendMessageW(prompt.text, EM_SETLIMITTEXT, prompt.request.max_length, 0);
    SendMessageW(prompt.text, EM_SETSEL, 0, -1);
    const int buttons = height - margin - line - scale(4);
    child(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, width - margin - 2 * button - scale(8),
          buttons, button, line + scale(4), IDOK, prompt.font);
    child(L"BUTTON", L"Cancel", WS_TABSTOP, width - margin - button, buttons, button,
          line + scale(4), IDCANCEL, prompt.font);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(prompt.text);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
}
} // namespace

void ask_text(TextRequest request, std::function<void(std::optional<std::wstring>)> done) {
    std::thread([request = std::move(request), done = std::move(done)]() mutable {
        Prompt prompt{std::move(request)};
        run(prompt);
        DeleteObject(prompt.font);
        DeleteObject(prompt.bold);
        done(std::move(prompt.result));
    }).detach();
}
} // namespace xs::surface
