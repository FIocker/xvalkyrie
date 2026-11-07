#include "runtime/durango/winrt/application.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/durango/runtime.hpp"
#include "runtime/durango/winrt/collection.hpp"
#include "runtime/durango/winrt/object.hpp"
#include <atomic>
#include <corewindow.h>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <intrin.h>
#include <memory>
#include <mutex>
#include <vector>
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

// The arguments of the window events the shell raises when the title's window comes up
class ActivatedArgs : public Class<ActivatedArgs, IWindowActivatedEventArgs, ICoreWindowEventArgs> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Core.WindowActivatedEventArgs";
    STDMETHODIMP get_WindowActivationState(CoreWindowActivationState *value) override {
        *value = CoreWindowActivationState_CodeActivated;
        return S_OK;
    }
    STDMETHODIMP get_Handled(boolean *value) override {
        *value = handled_;
        return S_OK;
    }
    STDMETHODIMP put_Handled(boolean value) override {
        handled_ = value;
        return S_OK;
    }

  private:
    boolean handled_ = false;
};
class VisibilityArgs
    : public Class<VisibilityArgs, IVisibilityChangedEventArgs, ICoreWindowEventArgs> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Core.VisibilityChangedEventArgs";
    STDMETHODIMP get_Visible(boolean *value) override {
        *value = true;
        return S_OK;
    }
    STDMETHODIMP get_Handled(boolean *value) override {
        *value = handled_;
        return S_OK;
    }
    STDMETHODIMP put_Handled(boolean value) override {
        handled_ = value;
        return S_OK;
    }

  private:
    boolean handled_ = false;
};

class Dispatcher : public Class<Dispatcher, ICoreDispatcher> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Core.CoreDispatcher";
    explicit Dispatcher(TitleWindow &window) : window_(window), thread_(GetCurrentThreadId()) {}
    // what ProcessEvents runs next on the thread that calls it: window events
    void post(std::function<void()> work) {
        const std::lock_guard lock(mutex_);
        pending_.push_back(std::move(work));
    }
    STDMETHODIMP get_HasThreadAccess(boolean *value) override {
        *value = GetCurrentThreadId() == thread_;
        return S_OK;
    }
    STDMETHODIMP ProcessEvents(CoreProcessEventsOption options) override {
        // The window's messages are handled on another thread; what there is to process
        // here are the events raised for the title, then only the options that wait wait.
        std::vector<std::function<void()>> work;
        {
            const std::lock_guard lock(mutex_);
            work.swap(pending_);
        }
        for (auto &item : work)
            item();
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
    std::mutex mutex_;
    std::vector<std::function<void()>> pending_;
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
    Dispatcher &dispatcher() { return *dispatcher_.Get(); }

    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) ||
            iid == __uuidof(IAgileObject) || iid == __uuidof(ICoreWindow)) {
            *object = static_cast<IConsoleWindow *>(this);
            AddRef();
            return S_OK;
        }
        if (iid == __uuidof(ICoreWindowInterop)) {
            *object = &interop_;
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
    XS_UNKNOWN(28) XS_UNKNOWN(29) XS_UNKNOWN(30) XS_UNKNOWN(32) XS_UNKNOWN(33)
    XS_UNKNOWN(34) XS_UNKNOWN(36) XS_UNKNOWN(38) XS_UNKNOWN(39)
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
    XS_EVENT(17) XS_EVENT(19) XS_EVENT(23) XS_EVENT(25) XS_EVENT(27) XS_EVENT(31) XS_EVENT(37)
    XS_EVENT(40) XS_EVENT(42)
#undef XS_EVENT
        // clang-format on
        // Activated (15) and VisibilityChanged (35), which the shell raises once the window is
        // up: activated, and visible. Titles wait for them before they start.
        STDMETHODIMP unknown15(void *handler, void *token) override {
        return raise_once<ActivatedArgs>(handler, token);
    }
    STDMETHODIMP unknown35(void *handler, void *token) override {
        return raise_once<VisibilityArgs>(handler, token);
    }
    template <class Args> HRESULT raise_once(void *handler, void *token) {
        if (!handler || !token)
            return E_POINTER;
        *static_cast<std::int64_t *>(token) = ++tokens_;
        ComPtr<IUnknown> held(static_cast<IUnknown *>(handler));
        ComPtr<Window> self(this);
        dispatcher_->post([held, self] {
            // ITypedEventHandler<CoreWindow *, Args *>::Invoke(sender, args), slot 3
            using Invoke = HRESULT(STDMETHODCALLTYPE *)(IUnknown *, IInspectable *, IInspectable *);
            auto args = Make<Args>();
            (*reinterpret_cast<Invoke **>(held.Get()))[3](
                held.Get(),
                reinterpret_cast<IInspectable *>(static_cast<IConsoleWindow *>(self.Get())),
                reinterpret_cast<IInspectable *>(args.Get()));
        });
        return S_OK;
    }

  private:
    std::atomic<ULONG> references_{1};
    std::atomic<std::int64_t> tokens_{0};
    TitleWindow host_;
    ComPtr<Dispatcher> dispatcher_;
    // public ICoreWindowInterop: the HWND, which engines such as Unity draw into
    struct Interop final : ICoreWindowInterop {
        Window *window;
        explicit Interop(Window *owner) : window(owner) {}
        STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
            return window->QueryInterface(iid, object);
        }
        STDMETHODIMP_(ULONG) AddRef() override { return window->AddRef(); }
        STDMETHODIMP_(ULONG) Release() override { return window->Release(); }
        STDMETHODIMP get_WindowHandle(HWND *handle) override {
            if (!handle)
                return E_POINTER;
            *handle = static_cast<HWND>(window->host_.native());
            return S_OK;
        }
        STDMETHODIMP put_MessageHandled(boolean) override { return S_OK; }
    } interop_{this};
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
        const auto result = activated_.Add(handler, token);
        // the shell raises Activated through the dispatcher: a title that subscribes only in
        // Run gets it at its next ProcessEvents
        if (SUCCEEDED(result) && activated_once_ && handler) {
            ComPtr<ITypedEventHandler<CoreApplicationView *, IActivatedEventArgs *>> held(handler);
            ComPtr<View> self(this);
            window_->dispatcher().post([held, self] {
                const auto arguments = Make<LaunchArguments>();
                held->Invoke(reinterpret_cast<ICoreApplicationView *>(self.Get()),
                             static_cast<IActivatedEventArgs *>(arguments.Get()));
            });
        }
        return result;
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
        activated_once_ = true;
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
    bool activated_once_ = false;
};

void step(const char *text) {
    std::fprintf(stderr, "[Runtime] %s\n", text);
    std::fflush(stderr);
}

// ICoreImmersiveApplication as the console has it: MainView only, where the desktop's has
// Views, CreateNewView and then MainView
MIDL_INTERFACE("1ada0e3e-e4a2-4123-b451-dc96bf800419")
IConsoleImmersiveApplication : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MainView(ICoreApplicationView * *value) = 0;
};

class Application : public Class<Application, IActivationFactory, ICoreApplication,
                                 IConsoleImmersiveApplication> {
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
        frame_watchdog(); // a title that never presents a frame counts as stalled from here
        if (SUCCEEDED(result))
            result = view->Run();
        step("CoreApplication: the view's Run returned");
        if (SUCCEEDED(result))
            result = view->Uninitialize();
        return result;
    }
    STDMETHODIMP RunWithActivationFactories(IGetActivationFactory *) override { return E_NOTIMPL; }
    // the console's ICoreImmersiveApplication: the one view there is, the main one
    STDMETHODIMP get_MainView(ICoreApplicationView **value) override {
        if (!value)
            return E_POINTER;
        if (!view_) {
            *value = nullptr;
            return E_FAIL;
        }
        return view_.CopyTo(value);
    }

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
