#include "runtime/durango/winrt/async.hpp"
#include <asyncinfo.h>
#include <atomic>
#include <inspectable.h>
#include <windows.h>

namespace xs::runtime::durango::winrt {
namespace {
// IAsyncOperation<T> and IAsyncAction have the same shape; only GetResults differs.
struct IOperation : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown *handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown **handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void **result) = 0;
};
class Operation final : public IOperation, public IAsyncInfo {
  public:
    Operation(IUnknown *result, bool action) : result_(result), action_(action) {}
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
            *object = static_cast<IOperation *>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left) {
            if (result_)
                result_->Release();
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
    STDMETHODIMP put_Completed(IUnknown *handler) override {
        if (!handler)
            return S_OK;
        // AsyncOperationCompletedHandler<T>::Invoke(operation, status)
        using Invoke = HRESULT(STDMETHODCALLTYPE *)(IUnknown *, IOperation *, AsyncStatus);
        const auto invoke = reinterpret_cast<Invoke>((*reinterpret_cast<void ***>(handler))[3]);
        return invoke(handler, this, AsyncStatus::Completed);
    }
    STDMETHODIMP get_Completed(IUnknown **handler) override {
        *handler = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetResults(void **result) override {
        if (action_)
            return S_OK;
        *result = result_;
        if (result_)
            result_->AddRef();
        return S_OK;
    }
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

  private:
    std::atomic<ULONG> references_{1};
    IUnknown *result_;
    bool action_;
};
} // namespace

void *make_completed_operation(void *result) {
    return static_cast<IOperation *>(new Operation(static_cast<IUnknown *>(result), false));
}
void *make_completed_action() { return static_cast<IOperation *>(new Operation(nullptr, true)); }
} // namespace xs::runtime::durango::winrt
