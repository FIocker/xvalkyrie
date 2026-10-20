#include "runtime/common/runtime.hpp"
#include "runtime/durango/runtime.hpp"

#ifdef _WIN32
#include "runtime/durango/winrt/application.hpp"
#include "runtime/durango/winrt/gamepad.hpp"
#include "runtime/durango/winrt/placeholder.hpp"
#include "runtime/durango/winrt/storage.hpp"
#include <cstdio>
#include <initializer_list>
#include <inspectable.h>
#include <roapi.h>
#include <string_view>
#include <windows.h>
#include <winstring.h>

namespace xs::runtime::durango {
namespace {
// Host classes that behave the same in any process. The rest of the host's classes want a
// packaged application around them, so the console versions are ours.
bool host_class(std::wstring_view name) {
    for (const std::wstring_view prefix :
         {L"Windows.Foundation.", L"Windows.Storage.Streams.", L"Windows.Data.",
          L"Windows.Globalization.", L"Windows.Security.Cryptography."})
        if (name.starts_with(prefix))
            return true;
    return false;
}

// vccorlib's GetActivationFactoryByPCWSTR: how C++/CX code reaches every WinRT class.
// Classes of the host are used as they are; the console ones have to be ours.
HRESULT __cdecl activation_factory(const wchar_t *name, const GUID &iid, void **factory) {
    if (auto *const own = winrt::application_class(name))
        return own->QueryInterface(iid, factory);
    if (auto *const own = winrt::input_class(name))
        return own->QueryInterface(iid, factory);
    if (auto *const own = winrt::storage_class(name))
        return own->QueryInterface(iid, factory);
    if (host_class(name)) {
        HSTRING_HEADER header;
        HSTRING text = nullptr;
        auto result =
            WindowsCreateStringReference(name, static_cast<UINT32>(wcslen(name)), &header, &text);
        if (SUCCEEDED(result))
            result = RoGetActivationFactory(text, iid, factory);
        if (SUCCEEDED(result))
            return result;
    }
    *factory = winrt::make_placeholder(name, iid);
    return S_OK;
}
} // namespace

void install_winrt(Registry &registry) {
    // titles built with Visual Studio 2012 use the first, later ones the second
    for (const auto *module : {"vccorlib110.dll", "vccorlib140.dll"})
        registry.add(module, "?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z",
                     reinterpret_cast<void *>(&activation_factory));
}
} // namespace xs::runtime::durango
#else
namespace xs::runtime::durango {
void install_winrt(Registry &) {}
} // namespace xs::runtime::durango
#endif
