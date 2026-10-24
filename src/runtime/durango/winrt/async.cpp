#include "runtime/durango/winrt/async.hpp"
#include <asyncinfo.h>
#include <atomic>
#include <cstring>
#include <inspectable.h>
#include <mutex>
#include <string>
#include <utility>
#include <windows.h>
#include <winstring.h>

namespace xs::runtime::durango::winrt {
namespace {
// IAsyncOperation<T> and IAsyncAction have the same shape; only GetResults differs.
struct IOperation : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void **result) = 0;
};
// IAsyncOperationWithProgress<T, P>: the progress handler comes first
struct IProgressOperation : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Progress(IUnknown *handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Progress(IUnknown **handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void **result) = 0;
};
enum class Result { object, value, text, none };
class Operation final : public IOperation, public IProgressOperation, public IAsyncInfo {
  public:
    Operation(IUnknown *result, bool action, HRESULT error = S_OK)
        : result_(result), kind_(action ? Result::none : Result::object), error_(error) {}
    Operation(unsigned long long value, unsigned size)
        : kind_(Result::value), value_(value), size_(size) {}
    explicit Operation(bool progress, HRESULT error) : progress_(progress), error_(error) {}
    Operation() : kind_(Result::text), done_(false) {}
    // The interface ID of IAsyncOperation<T> depends on T, so anything that is not
    // IAsyncInfo is taken to be the operation itself.
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        constexpr GUID marshal{0x00000003, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid == marshal)
            return E_NOINTERFACE;
        if (iid == __uuidof(IAsyncInfo))
            *object = static_cast<IAsyncInfo *>(this);
        else
            *object = self();
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left) {
            if (result_)
                result_->Release();
            if (handler_)
                handler_->Release();
            delete this;
        }
        return left;
    }
    STDMETHODIMP GetIids(ULONG *count, IID **iids) override {
        *count = 0;
        *iids = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetRuntimeClassName(HSTRING *name) override {
        *name = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetTrustLevel(TrustLevel *level) override {
        *level = BaseTrust;
        return S_OK;
    }
    STDMETHODIMP put_Progress(IUnknown *) override { return S_OK; }
    STDMETHODIMP get_Progress(IUnknown **handler) override {
        *handler = nullptr;
        return S_OK;
    }
    STDMETHODIMP put_Completed(IUnknown *handler) override {
        if (!handler)
            return S_OK;
        {
            const std::lock_guard lock(mutex_);
            if (!done_) {
                handler->AddRef();
                handler_ = handler;
                return S_OK;
            }
        }
        return invoke(handler);
    }
    STDMETHODIMP get_Completed(IUnknown **handler) override {
        *handler = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetResults(void **result) override {
        const std::lock_guard lock(mutex_);
        if (!done_)
            return E_ILLEGAL_METHOD_CALL;
        if (FAILED(error_))
            return error_;
        switch (kind_) {
        case Result::none:
            return S_OK;
        case Result::value:
            std::memcpy(result, &value_, size_);
            return S_OK;
        case Result::text:
            return WindowsCreateString(text_.c_str(), static_cast<UINT32>(text_.size()),
                                       reinterpret_cast<HSTRING *>(result));
        case Result::object:
            *result = result_;
            if (result_)
                result_->AddRef();
        }
        return S_OK;
    }
    STDMETHODIMP get_Id(unsigned *id) override {
        *id = 1;
        return S_OK;
    }
    STDMETHODIMP get_Status(AsyncStatus *status) override {
        const std::lock_guard lock(mutex_);
        *status = this->status();
        return S_OK;
    }
    STDMETHODIMP get_ErrorCode(HRESULT *code) override {
        *code = error_;
        return S_OK;
    }
    STDMETHODIMP Cancel() override { return S_OK; }
    STDMETHODIMP Close() override { return S_OK; }

    void finish(std::wstring text) {
        IUnknown *handler = nullptr;
        {
            const std::lock_guard lock(mutex_);
            text_ = std::move(text);
            done_ = true;
            handler = std::exchange(handler_, nullptr);
        }
        if (handler) {
            invoke(handler);
            handler->Release();
        }
    }
    IInspectable *self() {
        return progress_ ? static_cast<IInspectable *>(static_cast<IProgressOperation *>(this))
                         : static_cast<IOperation *>(this);
    }

  private:
    // AsyncOperationCompletedHandler<T>::Invoke(operation, status)
    HRESULT invoke(IUnknown *handler) {
        using Invoke = HRESULT(STDMETHODCALLTYPE *)(IUnknown *, IInspectable *, AsyncStatus);
        const auto call = reinterpret_cast<Invoke>((*reinterpret_cast<void ***>(handler))[3]);
        AsyncStatus now;
        {
            const std::lock_guard lock(mutex_);
            now = status();
        }
        return call(handler, self(), now);
    }
    AsyncStatus status() const {
        return !done_           ? AsyncStatus::Started
               : FAILED(error_) ? AsyncStatus::Error
                                : AsyncStatus::Completed;
    }
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    IUnknown *result_ = nullptr, *handler_ = nullptr;
    Result kind_ = Result::none;
    bool progress_ = false, done_ = true;
    HRESULT error_ = S_OK;
    unsigned long long value_ = 0;
    unsigned size_ = 0;
    std::wstring text_;
};
} // namespace

void *make_completed_operation(void *result) {
    return static_cast<IOperation *>(new Operation(static_cast<IUnknown *>(result), false));
}
void *make_failed_action(long error) {
    return static_cast<IOperation *>(new Operation(nullptr, true, error));
}
void *make_failed_operation(long error) {
    return static_cast<IOperation *>(new Operation(nullptr, false, error));
}
void *make_failed_progress_operation(long error) { return (new Operation(true, error))->self(); }
void *make_completed_value(unsigned long long value, unsigned size) {
    return static_cast<IOperation *>(new Operation(value, size));
}
void *make_completed_action() { return static_cast<IOperation *>(new Operation(nullptr, true)); }
void *make_text_operation(std::function<void(std::function<void(std::wstring)>)> start) {
    auto *const operation = new Operation;
    operation->AddRef(); // for `finish`, until it is called
    start([operation](std::wstring text) {
        operation->finish(std::move(text));
        operation->Release();
    });
    return static_cast<IOperation *>(operation);
}
} // namespace xs::runtime::durango::winrt
