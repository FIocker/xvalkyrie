#include "runtime/durango/winrt/collection.hpp"
#include <algorithm>
#include <atomic>
#include <inspectable.h>
#include <utility>
#include <windows.h>

namespace xs::runtime::durango::winrt {
namespace {
struct IObjectVectorView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned index, IInspectable **item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned *size) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *item, unsigned *index,
                                              boolean *found) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned start, unsigned capacity,
                                              IInspectable **items, unsigned *actual) = 0;
};
class VectorView final : public IObjectVectorView {
  public:
    explicit VectorView(std::vector<IInspectable *> items) : items_(std::move(items)) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        constexpr GUID marshal{0x00000003, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid == marshal)
            return E_NOINTERFACE;
        *object = this;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left) {
            for (auto *item : items_)
                item->Release();
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
    STDMETHODIMP GetAt(unsigned index, IInspectable **item) override {
        if (index >= items_.size())
            return E_BOUNDS;
        *item = items_[index];
        (*item)->AddRef();
        return S_OK;
    }
    STDMETHODIMP get_Size(unsigned *size) override {
        *size = static_cast<unsigned>(items_.size());
        return S_OK;
    }
    STDMETHODIMP IndexOf(IInspectable *item, unsigned *index, boolean *found) override {
        const auto position = std::find(items_.begin(), items_.end(), item);
        *found = position != items_.end();
        *index = *found ? static_cast<unsigned>(position - items_.begin()) : 0;
        return S_OK;
    }
    STDMETHODIMP GetMany(unsigned start, unsigned capacity, IInspectable **items,
                         unsigned *actual) override {
        unsigned count = 0;
        for (; count < capacity && start + count < items_.size(); ++count) {
            items[count] = items_[start + count];
            items[count]->AddRef();
        }
        *actual = count;
        return S_OK;
    }

  private:
    std::atomic<ULONG> references_{1};
    std::vector<IInspectable *> items_;
};
} // namespace

void *make_vector_view(std::vector<IInspectable *> items) {
    return static_cast<IObjectVectorView *>(new VectorView(std::move(items)));
}
} // namespace xs::runtime::durango::winrt
