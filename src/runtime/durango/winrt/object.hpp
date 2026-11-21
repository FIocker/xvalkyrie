#pragma once
#include "runtime/durango/winrt/placeholder.hpp"
#include <wrl.h>

namespace xv::runtime::durango::winrt {
// Base of our WinRT classes. `Derived::name` is the runtime class name. An interface the
// class does not implement is answered by a placeholder, so that the call that needs it
// gets reported instead of the title giving up on the cast.
template <class Derived, class... Interfaces>
class Class
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRt>,
                                          Interfaces...> {
    using Base =
        Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRt>,
                                     Interfaces...>;

  public:
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        const auto result = Base::QueryInterface(iid, object);
        return result == E_NOINTERFACE ? placeholder_interface(Derived::name, iid, object) : result;
    }
    STDMETHODIMP GetRuntimeClassName(HSTRING *name) override {
        return WindowsCreateString(Derived::name, static_cast<UINT32>(wcslen(Derived::name)), name);
    }
    STDMETHODIMP GetTrustLevel(TrustLevel *level) override {
        *level = BaseTrust;
        return S_OK;
    }
};
} // namespace xv::runtime::durango::winrt
