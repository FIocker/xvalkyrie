#include "runtime/durango/winrt/system_ui.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/winrt/async.hpp"
#include "runtime/durango/winrt/collection.hpp"
#include "runtime/durango/winrt/object.hpp"
#include "runtime/durango/winrt/placeholder.hpp"
#include <activation.h>
#include <cstdio>
#include <mmdeviceapi.h>
#include <optional>
#include <string>
#include <thread>
#include <windows.h>
#include <winstring.h>
#include <wrl/wrappers/corewrappers.h>

namespace xs::runtime::durango::winrt {
namespace {
using Microsoft::WRL::Make;
using Microsoft::WRL::Wrappers::HString;

// IKeyboardOptions: a property bag the title fills before it
// asks for the keyboard
MIDL_INTERFACE("74782F58-DA5D-4E31-9E6E-BD10C91F1BE7")
IKeyboardOptions : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DefaultText(HSTRING * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DefaultText(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TitleText(HSTRING * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_TitleText(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DescriptionText(HSTRING * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DescriptionText(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxLength(UINT32 * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MaxLength(UINT32 value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LengthIndicatorVisibility(boolean * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_LengthIndicatorVisibility(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LengthIndicatorThreshold(UINT32 * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_LengthIndicatorThreshold(UINT32 value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InputScope(UINT32 * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_InputScope(UINT32 value) = 0;
};

class KeyboardOptions : public Class<KeyboardOptions, IKeyboardOptions> {
  public:
    static constexpr wchar_t name[] = L"Windows.Xbox.UI.KeyboardOptions";
    STDMETHODIMP get_DefaultText(HSTRING *value) override { return text_.CopyTo(value); }
    STDMETHODIMP put_DefaultText(HSTRING value) override { return text_.Set(value); }
    STDMETHODIMP get_TitleText(HSTRING *value) override { return title_.CopyTo(value); }
    STDMETHODIMP put_TitleText(HSTRING value) override { return title_.Set(value); }
    STDMETHODIMP get_DescriptionText(HSTRING *value) override { return description_.CopyTo(value); }
    STDMETHODIMP put_DescriptionText(HSTRING value) override { return description_.Set(value); }
    STDMETHODIMP get_MaxLength(UINT32 *value) override { return get(max_length_, value); }
    STDMETHODIMP put_MaxLength(UINT32 value) override { return put(max_length_, value); }
    STDMETHODIMP get_LengthIndicatorVisibility(boolean *value) override {
        return get(indicator_, value);
    }
    STDMETHODIMP put_LengthIndicatorVisibility(boolean value) override {
        return put(indicator_, value);
    }
    STDMETHODIMP get_LengthIndicatorThreshold(UINT32 *value) override {
        return get(threshold_, value);
    }
    STDMETHODIMP put_LengthIndicatorThreshold(UINT32 value) override {
        return put(threshold_, value);
    }
    STDMETHODIMP get_InputScope(UINT32 *value) override { return get(scope_, value); }
    STDMETHODIMP put_InputScope(UINT32 value) override { return put(scope_, value); }

  private:
    template <class T> static HRESULT get(T field, T *value) {
        if (!value)
            return E_POINTER;
        *value = field;
        return S_OK;
    }
    template <class T> static HRESULT put(T &field, T value) {
        field = value;
        return S_OK;
    }
    HString text_, title_, description_;
    UINT32 max_length_ = 0, threshold_ = 0, scope_ = 0;
    boolean indicator_ = false;
};
class KeyboardOptionsFactory : public Class<KeyboardOptionsFactory, IActivationFactory> {
  public:
    static constexpr wchar_t name[] = L"Windows.Xbox.UI.KeyboardOptions";
    STDMETHODIMP ActivateInstance(IInspectable **instance) override {
        if (!instance)
            return E_POINTER;
        *instance = Make<KeyboardOptions>().Detach();
        return S_OK;
    }
};

std::wstring text(HSTRING value) {
    UINT32 length = 0;
    const auto *raw = WindowsGetStringRawBuffer(value, &length);
    return {raw, length};
}
// VirtualKeyboardInputScope: Number 0x1d, Password 0x1f, TelephoneNumber 0x20
void *keyboard(surface::TextRequest request, std::uint32_t scope) {
    request.password = scope == 0x1f;
    request.digits = scope == 0x1d || scope == 0x20;
    const bool ask = console_settings().keyboard_dialog;
    std::fprintf(stderr, "[Runtime] system keyboard: \"%ls\"%s\n", request.title.c_str(),
                 ask ? "" : ", answered with the default text");
    std::fflush(stderr);
    return make_text_operation([request = std::move(request), ask](auto finish) mutable {
        if (!ask)
            return finish(request.text);
        auto fallback = request.text;
        // cancelled: the text the title suggested, so that it always gets a string back
        surface::ask_text(std::move(request),
                          [finish, fallback](std::optional<std::wstring> entered) {
                              finish(entered.value_or(fallback));
                          });
    });
}
// the id of the host's communications output, as WASAPI names it; COM on a thread of its own
std::wstring communications_output() {
    std::wstring id;
    std::thread([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
            return;
        IMMDeviceEnumerator *devices = nullptr;
        IMMDevice *device = nullptr;
        wchar_t *name = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&devices))) &&
            SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eCommunications, &device)) &&
            SUCCEEDED(device->GetId(&name)))
            id = name;
        CoTaskMemFree(name);
        if (device)
            device->Release();
        if (devices)
            devices->Release();
        CoUninitialize();
    }).join();
    return id;
}
} // namespace

std::wstring console_string(std::wstring_view key) {
    const auto &settings = console_settings();
    if (key == L"gamertag")
        return settings.gamertag;
    if (key == L"xbox_user_id")
        return settings.xbox_user_id;
    if (key == L"audio_device_id") {
        static const auto id = communications_output();
        return id;
    }
    return {};
}
std::uint32_t console_number(std::wstring_view key) {
    const auto &settings = console_settings();
    return key == L"gamerscore"  ? settings.gamerscore
           : key == L"age_group" ? settings.age_group
                                 : 0;
}
void *audio_devices() {
    if (!console_settings().headset)
        return make_vector_view({});
    return make_vector_view(
        {static_cast<IInspectable *>(shared_placeholder(L"Windows.Xbox.System.AudioDeviceInfo"))});
}
void *show_keyboard(HSTRING value, HSTRING title, HSTRING description, std::uint32_t scope) {
    return keyboard({text(title), text(description), text(value)}, scope);
}
void *show_keyboard_with_options(void *object) {
    auto *const unknown = static_cast<IInspectable *>(object);
    IKeyboardOptions *options = nullptr;
    if (!unknown || FAILED(unknown->QueryInterface(IID_PPV_ARGS(&options))))
        return make_failed_operation(E_INVALIDARG);
    HString value, title, description;
    UINT32 scope = 0, max_length = 0;
    options->get_DefaultText(value.GetAddressOf());
    options->get_TitleText(title.GetAddressOf());
    options->get_DescriptionText(description.GetAddressOf());
    options->get_InputScope(&scope);
    options->get_MaxLength(&max_length);
    options->Release();
    surface::TextRequest request{text(title.Get()), text(description.Get()), text(value.Get())};
    request.max_length = max_length;
    return keyboard(std::move(request), scope);
}
IInspectable *system_ui_class(std::wstring_view name) {
    static const auto options = Make<KeyboardOptionsFactory>();
    return name == KeyboardOptionsFactory::name ? static_cast<IActivationFactory *>(options.Get())
                                                : nullptr;
}
} // namespace xs::runtime::durango::winrt
