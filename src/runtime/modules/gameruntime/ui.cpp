#include "backends/surface/surface.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/gameruntime/gameruntime.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mmreg.h>
#include <mutex>
#include <objbase.h>
#include <optional>
#include <sapi.h>
#include <set>
#include <thread>
#include <utility>

// The system's own screens and voices (XGameUi, XAccessibility). Dialogs and the keyboard
// are windows of the host when Options > Console says so, and answer with their defaults
// otherwise; the screens of Xbox Live (profiles, achievements, invitations, the player
// picker) have nothing to show. Speech is the host's synthesizer.
namespace xv::runtime::modules::gameruntime {
namespace {
const char dialog_identity[] = "XGameUiShowMessageDialogAsync";
const char text_identity[] = "XGameUiShowTextEntryAsync";
const char shown_identity[] = "XGameUiShow";
const char picker_identity[] = "XGameUiShowPlayerPickerAsync";
const char web_identity[] = "XGameUiShowWebAuthenticationAsync";
std::atomic<std::uint32_t> hypotheses{0};

std::wstring wide(const char *text) {
    if (!text)
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size > 0 ? size - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
    return out;
}
std::string narrow(const std::wstring &text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}
std::vector<std::uint8_t> bytes_of(std::uint32_t value) {
    std::vector<std::uint8_t> bytes(sizeof value);
    std::memcpy(bytes.data(), &value, sizeof value);
    return bytes;
}

// A title may draw these screens itself (XGameUiSetUiCallbacks, XGameUiUiCallbacks of
// XGameUI.h). Its callback for a screen gets a handle and what the screen was asked with, on
// the work port of the operation's queue, and the operation ends when the title answers the
// handle with the screen's XGameUiSet...UiResponse.
enum class Screen {
    profile_card,
    player_picker,
    send_invite,
    achievements,
    activity_invite,
    message_dialog,
    error_dialog,
    text_entry
};
struct UiCallbacks {
    void *context;
    void *screens[8]; // in the order of Screen
};
struct Pending {
    Screen screen;
    Outcome outcome;
};
std::mutex ui_mutex;
UiCallbacks ui_callbacks{};
bool prefer_system_ui = true;
std::set<Pending *> pending;

// the title's callback for a screen and its context, if the title is the one to draw it
template <class Callback> std::pair<Callback *, void *> drawn_by_title(Screen screen, bool here) {
    const std::lock_guard lock(ui_mutex);
    auto *const callback = ui_callbacks.screens[static_cast<std::size_t>(screen)];
    return {here && prefer_system_ui ? nullptr : reinterpret_cast<Callback *>(callback),
            ui_callbacks.context};
}
HRESULT hand_over(XAsyncBlock *block, const void *identity, Screen screen,
                  std::function<void(Pending *, Queue *)> show) {
    if (!block)
        return E_INVALIDARG;
    return start(block, identity,
                 [screen, queue = block->queue, show = std::move(show)](Outcome outcome) {
                     auto *const handle = new Pending{screen, std::move(outcome)};
                     {
                         const std::lock_guard lock(ui_mutex);
                         pending.insert(handle);
                     }
                     show(handle, queue);
                 });
}
HRESULT respond(Pending *handle, Screen screen, std::vector<std::uint8_t> payload = {}) {
    {
        const std::lock_guard lock(ui_mutex);
        if (!pending.contains(handle) || handle->screen != screen)
            return E_INVALIDARG;
        pending.erase(handle);
    }
    handle->outcome(S_OK, std::move(payload));
    delete handle;
    return S_OK;
}
using Text = std::optional<std::string>;
Text text_of(const char *text) { return text ? Text(text) : std::nullopt; }
const char *raw(const Text &text) { return text ? text->c_str() : nullptr; }

HRESULT WINAPI set_ui_callbacks(void *, const UiCallbacks *callbacks, bool system_if_available) {
    const std::lock_guard lock(ui_mutex);
    ui_callbacks = callbacks ? *callbacks : UiCallbacks{};
    prefer_system_ui = system_if_available;
    return S_OK;
}

// Up to three buttons with the title's own words. The host's message box has fixed
// captions, so the text says which of them stands for which.
HRESULT WINAPI message_dialog(void *, XAsyncBlock *block, const char *title, const char *content,
                              const char *first, const char *second, const char *third,
                              std::uint32_t default_button, std::uint32_t cancel_button) {
    const bool ask = console_settings().keyboard_dialog;
    using Callback =
        void CALLBACK(Pending *, Queue *, const char *, const char *, const char *, const char *,
                      const char *, std::uint32_t, std::uint32_t, void *);
    if (const auto [callback, context] = drawn_by_title<Callback>(Screen::message_dialog, ask);
        callback)
        return hand_over(block, dialog_identity, Screen::message_dialog,
                         [=, texts = std::array{text_of(title), text_of(content), text_of(first),
                                                text_of(second), text_of(third)}](Pending *handle,
                                                                                  Queue *queue) {
                             callback(handle, queue, raw(texts[0]), raw(texts[1]), raw(texts[2]),
                                      raw(texts[3]), raw(texts[4]), default_button, cancel_button,
                                      context);
                         });
    std::fprintf(stderr, "[Runtime] message dialog: \"%s\": \"%s\"%s\n", title ? title : "",
                 content ? content : "", ask ? "" : ", answered with its default button");
    std::fflush(stderr);
    if (!ask)
        return finish(block, dialog_identity, S_OK, bytes_of(default_button));
    const int buttons = third ? 3 : second ? 2 : 1;
    auto text = wide(content);
    if (buttons > 1) {
        text += L"\n\n" + std::wstring(L"Yes: ") + wide(first) + L"\nNo: " + wide(second);
        if (third)
            text += L"\nCancel: " + wide(third);
    }
    return start(
        block, dialog_identity,
        [caption = wide(title), text, buttons, default_button, cancel_button](Outcome outcome) {
            std::thread([=] {
                const UINT kind = buttons == 3 ? MB_YESNOCANCEL : buttons == 2 ? MB_YESNO : MB_OK;
                const UINT initial = default_button == 2   ? MB_DEFBUTTON3
                                     : default_button == 1 ? MB_DEFBUTTON2
                                                           : MB_DEFBUTTON1;
                const int answer = MessageBoxW(nullptr, text.c_str(), caption.c_str(),
                                               kind | initial | MB_SETFOREGROUND | MB_TOPMOST);
                const std::uint32_t button = answer == IDYES || answer == IDOK    ? 0
                                             : answer == IDNO                     ? 1
                                             : answer == IDCANCEL && buttons == 3 ? 2
                                                                                  : cancel_button;
                outcome(S_OK, bytes_of(button));
            }).detach();
        });
}
HRESULT WINAPI message_dialog_result(void *, XAsyncBlock *block, std::uint32_t *button) {
    return result(block, dialog_identity, sizeof *button, button, nullptr);
}
HRESULT WINAPI message_dialog_response(void *, Pending *handle, std::uint32_t button) {
    return respond(handle, Screen::message_dialog, bytes_of(button));
}
HRESULT WINAPI error_dialog(void *, XAsyncBlock *block, HRESULT code, const char *service) {
    std::fprintf(stderr, "[Runtime] error dialog: 0x%08lx %s\n", static_cast<unsigned long>(code),
                 service ? service : "");
    std::fflush(stderr);
    using Callback = void CALLBACK(Pending *, Queue *, HRESULT, const char *, void *);
    if (const auto [callback, context] = drawn_by_title<Callback>(Screen::error_dialog, false);
        callback)
        return hand_over(block, shown_identity, Screen::error_dialog,
                         [=, text = text_of(service)](Pending *handle, Queue *queue) {
                             callback(handle, queue, code, raw(text), context);
                         });
    return finish(block, shown_identity, S_OK);
}
// The screens of Xbox Live have nothing behind them here: each is closed as soon as it is
// asked for, unless the title draws it.
HRESULT WINAPI profile_card(void *, XAsyncBlock *block, void *user, std::uint64_t player) {
    using Callback = void CALLBACK(Pending *, Queue *, void *, std::uint64_t, void *);
    if (const auto [callback, context] = drawn_by_title<Callback>(Screen::profile_card, false);
        callback)
        return hand_over(
            block, shown_identity, Screen::profile_card,
            [=](Pending *handle, Queue *queue) { callback(handle, queue, user, player, context); });
    return finish(block, shown_identity, S_OK);
}
HRESULT WINAPI achievements(void *, XAsyncBlock *block, void *user, std::uint32_t title) {
    using Callback = void CALLBACK(Pending *, Queue *, void *, std::uint32_t, void *);
    if (const auto [callback, context] = drawn_by_title<Callback>(Screen::achievements, false);
        callback)
        return hand_over(
            block, shown_identity, Screen::achievements,
            [=](Pending *handle, Queue *queue) { callback(handle, queue, user, title, context); });
    return finish(block, shown_identity, S_OK);
}
HRESULT WINAPI activity_invite(void *, XAsyncBlock *block, void *user) {
    using Callback = void CALLBACK(Pending *, Queue *, void *, void *);
    if (const auto [callback, context] = drawn_by_title<Callback>(Screen::activity_invite, false);
        callback)
        return hand_over(
            block, shown_identity, Screen::activity_invite,
            [=](Pending *handle, Queue *queue) { callback(handle, queue, user, context); });
    return finish(block, shown_identity, S_OK);
}
HRESULT WINAPI send_invite(void *, XAsyncBlock *block, void *user, const char *configuration,
                           const char *session_template, const char *session,
                           const char *invitation, const char *activation) {
    using Callback = void CALLBACK(Pending *, Queue *, void *, const char *, const char *,
                                   const char *, const char *, const char *, void *);
    if (const auto [callback, context] = drawn_by_title<Callback>(Screen::send_invite, false);
        callback)
        return hand_over(
            block, shown_identity, Screen::send_invite,
            [=, texts = std::array{text_of(configuration), text_of(session_template),
                                   text_of(session), text_of(invitation),
                                   text_of(activation)}](Pending *handle, Queue *queue) {
                callback(handle, queue, user, raw(texts[0]), raw(texts[1]), raw(texts[2]),
                         raw(texts[3]), raw(texts[4]), context);
            });
    return finish(block, shown_identity, S_OK);
}
// a screen with nothing behind it: closed as soon as it is asked for
HRESULT WINAPI shown(void *, XAsyncBlock *block) { return finish(block, shown_identity, S_OK); }
HRESULT WINAPI shown_result(void *, XAsyncBlock *block) {
    return result(block, shown_identity, 0, nullptr, nullptr);
}
template <Screen screen> HRESULT WINAPI shown_response(void *, Pending *handle) {
    return respond(handle, screen);
}
// XGameUiPlayerPickerInfo
struct PickerInfo {
    void *user;
    const char *prompt;
    std::uint32_t from_count;
    const std::uint64_t *from;
    std::uint32_t selected_count;
    const std::uint64_t *selected;
    std::uint32_t least, most;
};
// nobody to pick from: the user leaves the picker, unless the title draws it
HRESULT WINAPI player_picker(void *, XAsyncBlock *block, void *user, const char *prompt,
                             std::uint32_t from_count, const std::uint64_t *from,
                             std::uint32_t selected_count, const std::uint64_t *selected,
                             std::uint32_t least, std::uint32_t most) {
    using Callback = void CALLBACK(Pending *, Queue *, const PickerInfo *, void *);
    if (const auto [callback, context] = drawn_by_title<Callback>(Screen::player_picker, false);
        callback)
        return hand_over(block, picker_identity, Screen::player_picker,
                         [=, text = text_of(prompt),
                          all = std::vector<std::uint64_t>(from, from + (from ? from_count : 0)),
                          chosen = std::vector<std::uint64_t>(
                              selected, selected + (selected ? selected_count : 0))](
                             Pending *handle, Queue *queue) {
                             const PickerInfo info{user,
                                                   raw(text),
                                                   static_cast<std::uint32_t>(all.size()),
                                                   all.data(),
                                                   static_cast<std::uint32_t>(chosen.size()),
                                                   chosen.data(),
                                                   least,
                                                   most};
                             callback(handle, queue, &info, context);
                         });
    return finish(block, picker_identity, E_ABORT);
}
HRESULT WINAPI player_picker_response(void *, Pending *handle, std::uint32_t count,
                                      const std::uint64_t *players) {
    std::vector<std::uint8_t> payload(players ? count * sizeof *players : 0);
    if (!payload.empty())
        std::memcpy(payload.data(), players, payload.size());
    return respond(handle, Screen::player_picker, std::move(payload));
}
HRESULT WINAPI player_picker_count(void *, XAsyncBlock *block, std::uint32_t *count) {
    std::size_t bytes = 0;
    const auto status = result_size(block, &bytes);
    if (count)
        *count = static_cast<std::uint32_t>(bytes / sizeof(std::uint64_t));
    return status;
}
HRESULT WINAPI player_picker_result(void *, XAsyncBlock *block, std::uint32_t count,
                                    std::uint64_t *players, std::uint32_t *used) {
    std::size_t bytes = 0;
    const auto status =
        result(block, picker_identity, count * sizeof(std::uint64_t), players, &bytes);
    if (used)
        *used = static_cast<std::uint32_t>(bytes / sizeof(std::uint64_t));
    return status;
}

// a page of a service to sign in to: there is no browser to show it, and the user leaves
HRESULT WINAPI web_authentication(void *, XAsyncBlock *block) {
    return finish(block, web_identity, E_ABORT);
}
HRESULT WINAPI web_authentication_size(void *, XAsyncBlock *block, std::size_t *size) {
    return result_size(block, size);
}
HRESULT WINAPI web_authentication_result(void *, XAsyncBlock *block, std::size_t, void *,
                                         void **data, std::size_t *used) {
    if (data)
        *data = nullptr;
    return result(block, web_identity, 0, nullptr, used);
}

// XGameUiTextEntryInputScope: number 29, password 31, telephone number 32
HRESULT WINAPI text_entry(void *, XAsyncBlock *block, const char *title, const char *description,
                          const char *suggested, std::uint32_t scope, std::uint32_t max_length) {
    surface::TextRequest request{wide(title), wide(description), wide(suggested)};
    request.password = scope == 31;
    request.digits = scope == 29 || scope == 32;
    request.max_length = max_length;
    const bool ask = console_settings().keyboard_dialog;
    std::fprintf(stderr, "[Runtime] system keyboard: \"%ls\"%s\n", request.title.c_str(),
                 ask ? "" : ", answered with the default text");
    std::fflush(stderr);
    const auto payload = [](const std::wstring &text) {
        const auto utf8 = narrow(text);
        return std::vector<std::uint8_t>(utf8.c_str(), utf8.c_str() + utf8.size() + 1);
    };
    using Callback = void CALLBACK(Pending *, Queue *, const char *, const char *, const char *,
                                   std::uint32_t, std::uint32_t, void *);
    if (const auto [callback, context] = drawn_by_title<Callback>(Screen::text_entry, ask);
        callback)
        return hand_over(
            block, text_identity, Screen::text_entry,
            [=, texts = std::array{text_of(title), text_of(description), text_of(suggested)}](
                Pending *handle, Queue *queue) {
                callback(handle, queue, raw(texts[0]), raw(texts[1]), raw(texts[2]), scope,
                         max_length, context);
            });
    if (!ask)
        return finish(block, text_identity, S_OK, payload(request.text));
    return start(block, text_identity, [request, payload](Outcome outcome) mutable {
        surface::ask_text(std::move(request), [outcome, payload](auto entered) {
            if (entered)
                outcome(S_OK, payload(*entered));
            else
                outcome(E_ABORT, {});
        });
    });
}
HRESULT WINAPI text_entry_size(void *, XAsyncBlock *block, std::uint32_t *size) {
    std::size_t bytes = 0;
    const auto status = result_size(block, &bytes);
    if (size)
        *size = static_cast<std::uint32_t>(bytes);
    return status;
}
HRESULT WINAPI text_entry_result(void *, XAsyncBlock *block, std::uint32_t size, char *buffer,
                                 std::uint32_t *used) {
    std::size_t bytes = 0;
    const auto status = result(block, text_identity, size, buffer, &bytes);
    if (used)
        *used = static_cast<std::uint32_t>(bytes);
    return status;
}

HRESULT WINAPI text_entry_response(void *, Pending *handle, const char *text) {
    if (!text)
        return E_INVALIDARG;
    return respond(handle, Screen::text_entry,
                   std::vector<std::uint8_t>(text, text + std::strlen(text) + 1));
}

// The keyboard that stays open while the title draws the text itself (XGameUiTextEntryOpen)
// and asks for its state every frame. The host's is a window that gives its text once, when
// it is closed: the state then says the text changed and the keyboard was dismissed. Without
// that window it is dismissed from the start, with the text it was opened with.
struct TextEntry {
    std::mutex mutex;
    std::string text;
    bool changed = false, dismissed = false;
};
using TextEntryHandle = std::shared_ptr<TextEntry>;
// XGameUiTextEntryOptions
struct TextEntryOptions {
    std::uint32_t scope, position, visibility;
};
HRESULT WINAPI text_entry_open(void *, const TextEntryOptions *options, std::uint32_t max_length,
                               const char *initial, std::uint32_t, TextEntryHandle **handle) {
    if (!options || !handle)
        return E_POINTER;
    auto entry = std::make_shared<TextEntry>();
    entry->text = initial ? initial : "";
    if (console_settings().keyboard_dialog) {
        surface::TextRequest request{{}, {}, wide(initial)};
        request.password = options->scope == 31;
        request.digits = options->scope == 29 || options->scope == 32;
        request.max_length = max_length;
        surface::ask_text(std::move(request), [entry](auto entered) {
            const std::lock_guard lock(entry->mutex);
            if (entered) {
                entry->text = narrow(*entered);
                entry->changed = true;
            }
            entry->dismissed = true;
        });
    } else {
        entry->dismissed = true;
    }
    *handle = new TextEntryHandle(std::move(entry));
    return S_OK;
}
void WINAPI text_entry_close(void *, TextEntryHandle *handle) { delete handle; }
// XGameUiTextEntryChangeTypeFlags: text changed 1, dismissed 2. The cursor is after the text,
// and nothing is being composed.
HRESULT WINAPI text_entry_state(void *, TextEntryHandle *handle, std::uint32_t *change,
                                std::uint32_t *cursor, std::uint32_t *clause_start,
                                std::uint32_t *clause_end, std::uint32_t size, char *buffer) {
    if (!handle || !change)
        return E_POINTER;
    auto &entry = **handle;
    const std::lock_guard lock(entry.mutex);
    const auto status = copy_text(entry.text, size, buffer, nullptr);
    if (FAILED(status))
        return status;
    *change = (entry.changed ? 1u : 0u) | (entry.dismissed ? 2u : 0u);
    entry.changed = false;
    if (cursor)
        *cursor = static_cast<std::uint32_t>(entry.text.size());
    for (auto *const clause : {clause_start, clause_end})
        if (clause)
            *clause = 0;
    return S_OK;
}
// XGameUiTextEntryExtents: the part of the screen the keyboard covers, which is none of it
HRESULT WINAPI text_entry_extents(void *, TextEntryHandle *handle, float *extents) {
    if (!handle || !extents)
        return E_POINTER;
    extents[0] = extents[1] = extents[2] = extents[3] = 0;
    return S_OK;
}
HRESULT WINAPI text_entry_update(void *, TextEntryHandle *handle, std::uint32_t) {
    return handle ? S_OK : E_POINTER;
}

HRESULT WINAPI succeeded(void *) { return S_OK; }
// XClosedCaptionProperties: captions are off, and would be white on black at the usual size
struct ClosedCaptions {
    std::uint8_t background[4], font[4], window[4]; // alpha, red, green, blue
    std::uint32_t font_edge, font_style;
    float font_scale;
    bool enabled;
};
HRESULT WINAPI closed_captions(void *, ClosedCaptions *properties) {
    if (!properties)
        return E_POINTER;
    *properties = {{255, 0, 0, 0}, {255, 255, 255, 255}, {0, 0, 0, 0}, 0, 0, 1.0f, false};
    return S_OK;
}
HRESULT WINAPI high_contrast(void *, std::uint32_t *mode) {
    if (!mode)
        return E_POINTER;
    *mode = 0; // off
    return S_OK;
}
// speech to text has no overlay to write on; a hypothesis is only numbered
HRESULT WINAPI begin_hypothesis(void *, const char *, const char *, std::uint32_t,
                                std::uint32_t *id) {
    if (!id)
        return E_POINTER;
    *id = ++hypotheses;
    return S_OK;
}

// XSpeechSynthesizer: the host's voice, into a WAV file in memory
struct Synthesizer {
} synthesizer;
struct Speech {
    std::vector<std::uint8_t> wave;
};
constexpr std::uint32_t speech_rate = 22050;
// 16-bit mono samples of `text` spoken, on a thread of its own for COM; none if the host
// has no voice
std::vector<std::uint8_t> speak(const std::wstring &text, bool ssml) {
    std::vector<std::uint8_t> samples;
    std::thread([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
            return;
        ISpVoice *voice = nullptr;
        ISpStream *stream = nullptr;
        IStream *memory = nullptr;
        WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, speech_rate, speech_rate * 2, 2, 16, 0};
        if (SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice))) &&
            SUCCEEDED(
                CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream))) &&
            SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &memory)) &&
            SUCCEEDED(stream->SetBaseStream(memory, SPDFID_WaveFormatEx, &format)) &&
            SUCCEEDED(voice->SetOutput(stream, TRUE)) &&
            SUCCEEDED(
                voice->Speak(text.c_str(), ssml ? SPF_PARSE_SSML : SPF_IS_NOT_XML, nullptr))) {
            STATSTG size{};
            ULONG read = 0;
            if (SUCCEEDED(memory->Stat(&size, STATFLAG_NONAME)) &&
                SUCCEEDED(memory->Seek({}, STREAM_SEEK_SET, nullptr))) {
                samples.resize(static_cast<std::size_t>(size.cbSize.QuadPart));
                memory->Read(samples.data(), static_cast<ULONG>(samples.size()), &read);
                samples.resize(read);
            }
        }
        if (memory)
            memory->Release();
        if (stream)
            stream->Release();
        if (voice)
            voice->Release();
        CoUninitialize();
    }).join();
    return samples;
}
std::vector<std::uint8_t> wave_file(const std::vector<std::uint8_t> &samples) {
    const auto size = static_cast<std::uint32_t>(samples.size());
    std::vector<std::uint8_t> file(44 + samples.size());
    const auto put = [&](std::size_t at, std::uint32_t value, std::size_t bytes = 4) {
        std::memcpy(file.data() + at, &value, bytes);
    };
    std::memcpy(file.data(), "RIFF", 4);
    put(4, 36 + size);
    std::memcpy(file.data() + 8, "WAVEfmt ", 8);
    put(16, 16);
    put(20, WAVE_FORMAT_PCM, 2);
    put(22, 1, 2);
    put(24, speech_rate);
    put(28, speech_rate * 2);
    put(32, 2, 2);
    put(34, 16, 2);
    std::memcpy(file.data() + 36, "data", 4);
    put(40, size);
    std::memcpy(file.data() + 44, samples.data(), samples.size());
    return file;
}
HRESULT WINAPI synthesizer_create(void *, Synthesizer **handle) {
    if (!handle)
        return E_POINTER;
    *handle = &synthesizer;
    return S_OK;
}
HRESULT stream_of(const char *text, Speech **stream, bool ssml) {
    if (!text || !stream)
        return E_POINTER;
    *stream = new Speech{wave_file(speak(wide(text), ssml))};
    return S_OK;
}
HRESULT WINAPI stream_from_text(void *, Synthesizer *, const char *text, Speech **stream) {
    return stream_of(text, stream, false);
}
HRESULT WINAPI stream_from_ssml(void *, Synthesizer *, const char *text, Speech **stream) {
    return stream_of(text, stream, true);
}
HRESULT WINAPI close_stream(void *, Speech *stream) {
    delete stream;
    return S_OK;
}
HRESULT WINAPI stream_size(void *, Speech *stream, std::size_t *size) {
    if (!stream || !size)
        return E_POINTER;
    *size = stream->wave.size();
    return S_OK;
}
HRESULT WINAPI stream_data(void *, Speech *stream, std::size_t size, void *buffer,
                           std::size_t *used) {
    if (!stream || !buffer)
        return E_POINTER;
    if (size < stream->wave.size())
        return e_not_sufficient_buffer;
    std::memcpy(buffer, stream->wave.data(), stream->wave.size());
    if (used)
        *used = stream->wave.size();
    return S_OK;
}
} // namespace

void install_ui() {
    implement("XGameUiShowMessageDialogAsync", &message_dialog);
    implement("XGameUiShowMessageDialogResult", &message_dialog_result);
    implement("XGameUiShowErrorDialogAsync", &error_dialog);
    implement("XGameUiShowAchievementsAsync", &achievements);
    implement("XGameUiShowPlayerProfileCardAsync", &profile_card);
    implement("XGameUiShowSendGameInviteAsync", &send_invite);
    implement("XGameUiShowMultiplayerActivityGameInviteAsync", &activity_invite);
    implement("XGameUiSetUiCallbacks", &set_ui_callbacks);
    implement("XGameUiSetMessageDialogUiResponse", &message_dialog_response);
    implement("XGameUiSetPlayerPickerUiResponse", &player_picker_response);
    implement("XGameUiSetTextEntryUiResponse", &text_entry_response);
    implement("XGameUiSetPlayerProfileCardUiResponse", &shown_response<Screen::profile_card>);
    implement("XGameUiSetSendGameInviteUiResponse", &shown_response<Screen::send_invite>);
    implement("XGameUiSetAchievementsUiResponse", &shown_response<Screen::achievements>);
    implement("XGameUiSetMultiplayerActivityGameInviteUiResponse",
              &shown_response<Screen::activity_invite>);
    implement("XGameUiSetErrorDialogUiResponse", &shown_response<Screen::error_dialog>);
    implement("XGameUiTextEntryOpen", &text_entry_open);
    implement("XGameUiTextEntryClose", &text_entry_close);
    implement("XGameUiTextEntryGetState", &text_entry_state);
    implement("XGameUiTextEntryGetExtents", &text_entry_extents);
    implement("XGameUiTextEntryUpdatePositionHint", &text_entry_update);
    implement("XGameUiTextEntryUpdateVisibility", &text_entry_update);
    implement("XGameUiShowErrorDialogResult", &shown_result);
    implement("XGameUiShowAchievementsResult", &shown_result);
    implement("XGameUiShowPlayerProfileCardResult", &shown_result);
    implement("XGameUiShowSendGameInviteResult", &shown_result);
    implement("XGameUiShowMultiplayerActivityGameInviteResult", &shown_result);
    implement("XGameUiShowPlayerPickerAsync", &player_picker);
    implement("XGameUiShowPlayerPickerResultCount", &player_picker_count);
    implement("XGameUiShowPlayerPickerResult", &player_picker_result);
    implement("XGameUiShowTextEntryAsync", &text_entry);
    implement("XGameUiShowTextEntryResultSize", &text_entry_size);
    implement("XGameUiShowTextEntryResult", &text_entry_result);
    implement("XGameUiSetNotificationPositionHint", &succeeded);
    implement("XGameUiShowStateShareAsync", &shown);
    implement("XGameUiShowStateShareResult", &shown_result);
    implement("XGameUiShowWebAuthenticationAsync", &web_authentication);
    implement("XGameUiShowWebAuthenticationWithOptionsAsync", &web_authentication);
    implement("XGameUiShowWebAuthenticationResultSize", &web_authentication_size);
    implement("XGameUiShowWebAuthenticationResult", &web_authentication_result);
    implement("XClosedCaptionGetProperties", &closed_captions);
    implement("XClosedCaptionSetEnabled", &succeeded);
    implement("XHighContrastGetMode", &high_contrast);
    implement("XSpeechToTextSetPositionHint", &succeeded);
    implement("XSpeechToTextSendString", &succeeded);
    implement("XSpeechToTextBeginHypothesisString", &begin_hypothesis);
    implement("XSpeechToTextUpdateHypothesisString", &succeeded);
    implement("XSpeechToTextFinalizeHypothesisString", &succeeded);
    implement("XSpeechToTextCancelHypothesisString", &succeeded);
    // the host's voices are not listed: the default one speaks, whichever is asked for
    implement("XSpeechSynthesizerEnumerateInstalledVoices", &succeeded);
    implement("XSpeechSynthesizerCreate", &synthesizer_create);
    implement("XSpeechSynthesizerCloseHandle", &succeeded);
    implement("XSpeechSynthesizerSetDefaultVoice", &succeeded);
    implement("XSpeechSynthesizerSetCustomVoice", &succeeded);
    implement("XSpeechSynthesizerCreateStreamFromText", &stream_from_text);
    implement("XSpeechSynthesizerCreateStreamFromSsml", &stream_from_ssml);
    implement("XSpeechSynthesizerCloseStreamHandle", &close_stream);
    implement("XSpeechSynthesizerGetStreamDataSize", &stream_size);
    implement("XSpeechSynthesizerGetStreamData", &stream_data);
}
} // namespace xv::runtime::modules::gameruntime
