#include "runtime/durango/winrt/application.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/durango/winrt/object.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <memory>
#include <windows.applicationmodel.activation.h>
#include <windows.applicationmodel.core.h>
#include <windows.foundation.h>
#include <windows.ui.core.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

namespace xs::runtime::durango::winrt {
using xs::surface::TitleWindow;
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::EventSource;
using Microsoft::WRL::Make;
using Microsoft::WRL::Wrappers::HStringReference;
using namespace ABI::Windows::ApplicationModel;
using namespace ABI::Windows::ApplicationModel::Activation;
using namespace ABI::Windows::ApplicationModel::Core;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::UI::Core;
namespace Collections = ABI::Windows::Foundation::Collections;

// what a console reports whatever the host window measures
constexpr Rect bounds{0, 0, 1920, 1080};

// The action RunAsync returns: the handler has already run by then.
class CompletedAction : public Class<CompletedAction, IAsyncAction, IAsyncInfo> {
  public:
    static constexpr wchar_t name[] = L"Windows.Foundation.IAsyncAction";
    STDMETHODIMP put_Completed(IAsyncActionCompletedHandler *handler) override {
        return handler ? handler->Invoke(this, AsyncStatus::Completed) : S_OK;
    }
    STDMETHODIMP get_Completed(IAsyncActionCompletedHandler **handler) override {
        *handler = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetResults() override { return S_OK; }
    STDMETHODIMP get_Id(unsigned *id) override {
        *id = 1;
        return S_OK;
    }
    STDMETHODIMP get_Status(AsyncStatus *status) override {
        *status = AsyncStatus::Completed;
        return S_OK;
    }
    STDMETHODIMP get_ErrorCode(HRESULT *code) override {
        *code = S_OK;
        return S_OK;
    }
    STDMETHODIMP Cancel() override { return S_OK; }
    STDMETHODIMP Close() override { return S_OK; }
};

class Dispatcher : public Class<Dispatcher, ICoreDispatcher> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Core.CoreDispatcher";
    explicit Dispatcher(TitleWindow &window) : window_(window), thread_(GetCurrentThreadId()) {}
    STDMETHODIMP get_HasThreadAccess(boolean *value) override {
        *value = GetCurrentThreadId() == thread_;
        return S_OK;
    }
    STDMETHODIMP ProcessEvents(CoreProcessEventsOption options) override {
        if (GetCurrentThreadId() != thread_)
            return RPC_E_WRONG_THREAD;
        // The window's messages are handled on another thread, so there is nothing to
        // process here: only the options that wait have something to do.
        if (options == CoreProcessEventsOption_ProcessUntilQuit)
            window_.wait_closed();
        return S_OK;
    }
    // Runs the handler right away, on whatever thread asks.
    STDMETHODIMP RunAsync(CoreDispatcherPriority, IDispatchedHandler *handler,
                          IAsyncAction **action) override {
        if (!handler || !action)
            return E_POINTER;
        const auto result = handler->Invoke();
        if (FAILED(result))
            return result;
        return Make<CompletedAction>().CopyTo(action);
    }
    STDMETHODIMP RunIdleAsync(IIdleDispatchedHandler *, IAsyncAction **action) override {
        *action = nullptr;
        return E_NOTIMPL;
    }

  private:
    TitleWindow &window_;
    DWORD thread_;
};

// ICoreWindow as the console has it, which is not the desktop one: the bounds are the first
// method, the dispatcher the second and Activate the sixth. What the others are is not
// known yet.
struct IConsoleWindow : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Bounds(Rect *value) = 0;                 // 6
    virtual HRESULT STDMETHODCALLTYPE get_Dispatcher(ICoreDispatcher **value) = 0; // 7
    virtual HRESULT STDMETHODCALLTYPE unknown8(void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE unknown9(void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE unknown10(void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Activate() = 0; // 11
    // clang-format off
#define XS_UNKNOWN(Index) virtual HRESULT STDMETHODCALLTYPE unknown##Index(void *, void *) = 0;
    XS_UNKNOWN(12) XS_UNKNOWN(13) XS_UNKNOWN(14) XS_UNKNOWN(15) XS_UNKNOWN(16) XS_UNKNOWN(17)
    XS_UNKNOWN(18) XS_UNKNOWN(19) XS_UNKNOWN(20) XS_UNKNOWN(21) XS_UNKNOWN(22) XS_UNKNOWN(23)
    XS_UNKNOWN(24) XS_UNKNOWN(25) XS_UNKNOWN(26) XS_UNKNOWN(27) XS_UNKNOWN(28) XS_UNKNOWN(29)
    XS_UNKNOWN(30) XS_UNKNOWN(31) XS_UNKNOWN(32) XS_UNKNOWN(33) XS_UNKNOWN(34) XS_UNKNOWN(35)
    XS_UNKNOWN(36) XS_UNKNOWN(37) XS_UNKNOWN(38) XS_UNKNOWN(39) XS_UNKNOWN(40) XS_UNKNOWN(41)
    XS_UNKNOWN(42) XS_UNKNOWN(43) XS_UNKNOWN(44) XS_UNKNOWN(45) XS_UNKNOWN(46) XS_UNKNOWN(47)
#undef XS_UNKNOWN
    // clang-format on
};

class Window final : public IConsoleWindow {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Core.CoreWindow";
    Window() : host_(L"XScarlet", 1280, 720), dispatcher_(Make<Dispatcher>(host_)) {}
    TitleWindow &host() { return host_; }

    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) ||
            iid == __uuidof(IAgileObject) || iid == __uuidof(ICoreWindow)) {
            *object = static_cast<IConsoleWindow *>(this);
            AddRef();
            return S_OK;
        }
        return placeholder_interface(name, iid, object);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left)
            delete this;
        return left;
    }
    STDMETHODIMP GetIids(ULONG *count, IID **iids) override {
        *count = 0;
        *iids = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetRuntimeClassName(HSTRING *value) override {
        return WindowsCreateString(name, static_cast<UINT32>(wcslen(name)), value);
    }
    STDMETHODIMP GetTrustLevel(TrustLevel *level) override {
        *level = BaseTrust;
        return S_OK;
    }
    STDMETHODIMP get_Bounds(Rect *value) override {
        *value = bounds;
        return S_OK;
    }
    STDMETHODIMP get_Dispatcher(ICoreDispatcher **value) override {
        return dispatcher_.CopyTo(value);
    }
    // The host window stays hidden until the title presents its first frame.
    STDMETHODIMP Activate() override { return S_OK; }
    // clang-format off
#define XS_UNKNOWN(Index)                                                                          \
    STDMETHODIMP unknown##Index(void *first, void *second) override {                              \
        return unknown_method(name, "console ICoreWindow", Index, first, second,                   \
                              _ReturnAddress());                                                   \
    }
    XS_UNKNOWN(8) XS_UNKNOWN(9) XS_UNKNOWN(10)
    XS_UNKNOWN(12) XS_UNKNOWN(13) XS_UNKNOWN(14) XS_UNKNOWN(16) XS_UNKNOWN(18)
    XS_UNKNOWN(20) XS_UNKNOWN(21) XS_UNKNOWN(22) XS_UNKNOWN(24) XS_UNKNOWN(26)
    XS_UNKNOWN(28) XS_UNKNOWN(29) XS_UNKNOWN(30) XS_UNKNOWN(31) XS_UNKNOWN(32) XS_UNKNOWN(33)
    XS_UNKNOWN(34) XS_UNKNOWN(35) XS_UNKNOWN(36) XS_UNKNOWN(38) XS_UNKNOWN(39)
    XS_UNKNOWN(41) XS_UNKNOWN(43) XS_UNKNOWN(44) XS_UNKNOWN(45)
    XS_UNKNOWN(46) XS_UNKNOWN(47)
#undef XS_UNKNOWN
    // Event registrations (handler, token). Which events they are is not known yet, so
    // nothing raises them.
#define XS_EVENT(Index)                                                                            \
    STDMETHODIMP unknown##Index(void *, void *token) override {                                    \
        *static_cast<std::int64_t *>(token) = ++tokens_;                                           \
        return S_OK;                                                                               \
    }
    XS_EVENT(15) XS_EVENT(17) XS_EVENT(19) XS_EVENT(23) XS_EVENT(25) XS_EVENT(27) XS_EVENT(37)
    XS_EVENT(40) XS_EVENT(42)
#undef XS_EVENT
      // clang-format on

      private : std::atomic<ULONG> references_ {
          1
      };
    std::atomic<std::int64_t> tokens_{0};
    TitleWindow host_;
    ComPtr<Dispatcher> dispatcher_;
};

// the window of the one view, once CoreApplication::Run has created it
Window *current_window = nullptr;

class WindowStatics : public Class<WindowStatics, IActivationFactory, ICoreWindowStatic> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Core.CoreWindow";
    STDMETHODIMP ActivateInstance(IInspectable **instance) override {
        *instance = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetForCurrentThread(ICoreWindow **window) override {
        *window = reinterpret_cast<ICoreWindow *>(static_cast<IConsoleWindow *>(current_window));
        if (current_window)
            current_window->AddRef();
        return S_OK;
    }
};

class LaunchArguments
    : public Class<LaunchArguments, ILaunchActivatedEventArgs, IActivatedEventArgs> {
  public:
    static constexpr wchar_t name[] =
        L"Windows.ApplicationModel.Activation.LaunchActivatedEventArgs";
    STDMETHODIMP get_Kind(ActivationKind *value) override {
        *value = ActivationKind_Launch;
        return S_OK;
    }
    STDMETHODIMP get_PreviousExecutionState(ApplicationExecutionState *value) override {
        *value = ApplicationExecutionState_NotRunning;
        return S_OK;
    }
    STDMETHODIMP get_SplashScreen(ISplashScreen **value) override {
        *value = nullptr;
        return S_OK;
    }
    STDMETHODIMP get_Arguments(HSTRING *value) override {
        *value = nullptr;
        return S_OK;
    }
    STDMETHODIMP get_TileId(HSTRING *value) override {
        return WindowsCreateString(L"App", 3, value);
    }
};

class View : public Class<View, ICoreApplicationView> {
  public:
    static constexpr wchar_t name[] = L"Windows.ApplicationModel.Core.CoreApplicationView";
    View() { current_window = window_.Get(); }
    STDMETHODIMP get_CoreWindow(ICoreWindow **value) override {
        *value = reinterpret_cast<ICoreWindow *>(static_cast<IConsoleWindow *>(window_.Get()));
        window_->AddRef();
        return S_OK;
    }
    STDMETHODIMP
    add_Activated(ITypedEventHandler<CoreApplicationView *, IActivatedEventArgs *> *handler,
                  EventRegistrationToken *token) override {
        return activated_.Add(handler, token);
    }
    STDMETHODIMP remove_Activated(EventRegistrationToken token) override {
        return activated_.Remove(token);
    }
    STDMETHODIMP get_IsMain(boolean *value) override {
        *value = true;
        return S_OK;
    }
    STDMETHODIMP get_IsHosted(boolean *value) override {
        *value = false;
        return S_OK;
    }
    HRESULT activate() {
        const auto arguments = Make<LaunchArguments>();
        return activated_.InvokeAll(reinterpret_cast<ICoreApplicationView *>(this),
                                    static_cast<IActivatedEventArgs *>(arguments.Get()));
    }

  private:
    ComPtr<Window> window_ = [] {
        ComPtr<Window> window;
        window.Attach(new Window);
        return window;
    }();
    EventSource<ITypedEventHandler<CoreApplicationView *, IActivatedEventArgs *>> activated_;
};

void step(const char *text) {
    std::fprintf(stderr, "[Runtime] %s\n", text);
    std::fflush(stderr);
}

class Application : public Class<Application, IActivationFactory, ICoreApplication> {
  public:
    static constexpr wchar_t name[] = L"Windows.ApplicationModel.Core.CoreApplication";
    STDMETHODIMP ActivateInstance(IInspectable **instance) override {
        *instance = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP get_Id(HSTRING *value) override { return WindowsCreateString(L"App", 3, value); }
    STDMETHODIMP add_Suspending(IEventHandler<SuspendingEventArgs *> *handler,
                                EventRegistrationToken *token) override {
        return suspending_.Add(handler, token);
    }
    STDMETHODIMP remove_Suspending(EventRegistrationToken token) override {
        return suspending_.Remove(token);
    }
    STDMETHODIMP add_Resuming(IEventHandler<IInspectable *> *handler,
                              EventRegistrationToken *token) override {
        return resuming_.Add(handler, token);
    }
    STDMETHODIMP remove_Resuming(EventRegistrationToken token) override {
        return resuming_.Remove(token);
    }
    STDMETHODIMP get_Properties(Collections::IPropertySet **value) override {
        ComPtr<IInspectable> set;
        const auto result = RoActivateInstance(
            HStringReference(L"Windows.Foundation.Collections.PropertySet").Get(), &set);
        return FAILED(result) ? result : set.CopyTo(value);
    }
    STDMETHODIMP GetCurrentView(ICoreApplicationView **value) override {
        if (!view_)
            return E_FAIL;
        return view_.CopyTo(value);
    }
    // The whole life of the title: returns when its view's Run does.
    STDMETHODIMP Run(IFrameworkViewSource *source) override {
        if (!source)
            return E_POINTER;
        if (view_)
            return E_ILLEGAL_METHOD_CALL;
        view_ = Make<View>();
        ComPtr<IFrameworkView> view;
        auto result = source->CreateView(&view);
        ComPtr<ICoreWindow> window;
        if (SUCCEEDED(result))
            result = view_->get_CoreWindow(&window);
        step("CoreApplication: view created");
        if (SUCCEEDED(result))
            result = view->Initialize(view_.Get());
        if (SUCCEEDED(result))
            result = view->SetWindow(window.Get());
        if (SUCCEEDED(result))
            result = view->Load(HStringReference(L"").Get());
        step("CoreApplication: view loaded");
        if (SUCCEEDED(result))
            result = view_->activate();
        step("CoreApplication: activated, entering the view's Run");
        if (SUCCEEDED(result))
            result = view->Run();
        step("CoreApplication: the view's Run returned");
        if (SUCCEEDED(result))
            result = view->Uninitialize();
        return result;
    }
    STDMETHODIMP RunWithActivationFactories(IGetActivationFactory *) override { return E_NOTIMPL; }

  private:
    ComPtr<View> view_;
    EventSource<IEventHandler<SuspendingEventArgs *>> suspending_;
    EventSource<IEventHandler<IInspectable *>> resuming_;
};
} // namespace

TitleWindow *current_title_window() { return current_window ? &current_window->host() : nullptr; }

IInspectable *application_class(std::wstring_view name) {
    static const auto application = Make<Application>();
    static const auto window = Make<WindowStatics>();
    if (name == Application::name)
        return static_cast<IActivationFactory *>(application.Get());
    if (name == WindowStatics::name)
        return static_cast<IActivationFactory *>(window.Get());
    return nullptr;
}
} // namespace xs::runtime::durango::winrt
