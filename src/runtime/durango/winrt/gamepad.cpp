#include "runtime/durango/winrt/gamepad.hpp"
#include "input/input.hpp"
#include "runtime/durango/winrt/collection.hpp"
#include "runtime/durango/winrt/placeholder.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <inspectable.h>
#include <intrin.h>
#include <windows.h>
#include <winstring.h>

namespace xs::runtime::durango::winrt {
namespace {
constexpr wchar_t class_name[] = L"Windows.Xbox.Input.Gamepad";

// What IGamepad's third method fills in, as titles read it back.
struct RawReading {
    std::uint64_t timestamp;
    std::uint32_t buttons; // xs::input::Button bits
    float left_trigger, right_trigger;
    float left_x, left_y, right_x, right_y;
};
static_assert(sizeof(RawReading) == 40);

// Common part of the two objects here: reference counting, and placeholders for the
// interfaces nobody has looked at yet (the controller interface among them).
template <class Interface> class Object : public Interface {
  public:
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) ||
            iid == __uuidof(IAgileObject)) {
            *object = static_cast<Interface *>(this);
            this->AddRef();
            return S_OK;
        }
        return placeholder_interface(class_name, iid, object);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override { return --references_; } // lives with the process
    STDMETHODIMP GetIids(ULONG *count, IID **iids) override {
        *count = 0;
        *iids = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetRuntimeClassName(HSTRING *name) override {
        return WindowsCreateString(class_name, static_cast<UINT32>(wcslen(class_name)), name);
    }
    STDMETHODIMP GetTrustLevel(TrustLevel *level) override {
        *level = BaseTrust;
        return S_OK;
    }

  private:
    std::atomic<ULONG> references_{1};
};

// IGamepad, the default interface: the reading is its third method.
struct IGamepad : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE unknown6(void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE unknown7(void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRawCurrentReading(RawReading *reading) = 0;
    virtual HRESULT STDMETHODCALLTYPE unknown9(void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE unknown10(void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE unknown11(void *, void *) = 0;
};
class Gamepad final : public Object<IGamepad> {
  public:
    STDMETHODIMP GetRawCurrentReading(RawReading *reading) override {
        const auto state = input::poll();
        // once each: that the title reads the gamepad, and that it has seen a button
        static std::atomic<bool> read{false}, pressed{false};
        if (!read.exchange(true))
            std::fprintf(stderr, "[Runtime] the title reads the gamepad\n");
        if (state.buttons && !pressed.exchange(true))
            std::fprintf(stderr, "[Runtime] first gamepad button reached the title (0x%x)\n",
                         state.buttons);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        *reading = {static_cast<std::uint64_t>(now.QuadPart),
                    state.buttons,
                    state.left_trigger,
                    state.right_trigger,
                    state.left_x,
                    state.left_y,
                    state.right_x,
                    state.right_y};
        return S_OK;
    }
#define XS_UNKNOWN(Index)                                                                          \
    STDMETHODIMP unknown##Index(void *first, void *second) override {                              \
        return unknown_method(class_name, "IGamepad", Index, first, second, _ReturnAddress());     \
    }
    XS_UNKNOWN(6) XS_UNKNOWN(7) XS_UNKNOWN(9) XS_UNKNOWN(10) XS_UNKNOWN(11)
#undef XS_UNKNOWN
};
Gamepad gamepad;

// The statics: the list of gamepads and two events that never happen, since the one
// gamepad is always there.
struct IGamepadStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Gamepads(void **gamepads) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_GamepadAdded(void *handler, std::int64_t *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_GamepadAdded(std::int64_t token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_GamepadRemoved(void *handler, std::int64_t *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_GamepadRemoved(std::int64_t token) = 0;
};
class Statics final : public Object<IGamepadStatics> {
  public:
    // IActivationFactory is asked for by GetActivationFactory itself; the statics are what
    // titles want from this class.
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        constexpr GUID statics{
            0x8c4c1bcd, 0xfdab, 0x4bf8, {0xbd, 0xe9, 0xab, 0xb2, 0xb1, 0xd6, 0xfd, 0xd3}};
        if (object && iid == statics) {
            *object = static_cast<IGamepadStatics *>(this);
            AddRef();
            return S_OK;
        }
        return Object::QueryInterface(iid, object);
    }
    STDMETHODIMP get_Gamepads(void **gamepads) override {
        gamepad.AddRef();
        *gamepads = make_vector_view({static_cast<IGamepad *>(&gamepad)});
        return S_OK;
    }
    STDMETHODIMP add_GamepadAdded(void *, std::int64_t *token) override {
        *token = ++tokens_;
        return S_OK;
    }
    STDMETHODIMP remove_GamepadAdded(std::int64_t) override { return S_OK; }
    STDMETHODIMP add_GamepadRemoved(void *, std::int64_t *token) override {
        *token = ++tokens_;
        return S_OK;
    }
    STDMETHODIMP remove_GamepadRemoved(std::int64_t) override { return S_OK; }

  private:
    std::atomic<std::int64_t> tokens_{0};
};
Statics statics;
} // namespace

IInspectable *input_class(std::wstring_view name) {
    return name == class_name ? static_cast<IGamepadStatics *>(&statics) : nullptr;
}
} // namespace xs::runtime::durango::winrt
