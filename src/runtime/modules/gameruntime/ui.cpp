#include "backends/surface/surface.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/gameruntime/gameruntime.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mmreg.h>
#include <objbase.h>
#include <sapi.h>
#include <thread>

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

// Up to three buttons with the title's own words. The host's message box has fixed
// captions, so the text says which of them stands for which.
HRESULT WINAPI message_dialog(void *, XAsyncBlock *block, const char *title, const char *content,
                              const char *first, const char *second, const char *third,
                              std::uint32_t default_button, std::uint32_t cancel_button) {
    const bool ask = console_settings().keyboard_dialog;
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
HRESULT WINAPI error_dialog(void *, XAsyncBlock *block, HRESULT code, const char *context) {
    std::fprintf(stderr, "[Runtime] error dialog: 0x%08lx %s\n", static_cast<unsigned long>(code),
                 context ? context : "");
    std::fflush(stderr);
    return finish(block, shown_identity, S_OK);
}
// a screen with nothing behind it: closed as soon as it is asked for
HRESULT WINAPI shown(void *, XAsyncBlock *block) { return finish(block, shown_identity, S_OK); }
HRESULT WINAPI shown_result(void *, XAsyncBlock *block) {
    return result(block, shown_identity, 0, nullptr, nullptr);
}
// nobody to pick from: the user leaves the picker
HRESULT WINAPI player_picker(void *, XAsyncBlock *block) {
    return finish(block, picker_identity, E_ABORT);
}
HRESULT WINAPI player_picker_count(void *, XAsyncBlock *block, std::uint32_t *count) {
    if (count)
        *count = 0;
    return result(block, picker_identity, 0, nullptr, nullptr);
}
HRESULT WINAPI player_picker_result(void *, XAsyncBlock *block, std::uint32_t, std::uint64_t *,
                                    std::uint32_t *used) {
    if (used)
        *used = 0;
    return result(block, picker_identity, 0, nullptr, nullptr);
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
    implement("XGameUiShowAchievementsAsync", &shown);
    implement("XGameUiShowPlayerProfileCardAsync", &shown);
    implement("XGameUiShowSendGameInviteAsync", &shown);
    implement("XGameUiShowMultiplayerActivityGameInviteAsync", &shown);
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
