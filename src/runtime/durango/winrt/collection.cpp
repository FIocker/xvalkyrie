#include "runtime/durango/winrt/collection.hpp"
#include <algorithm>
#include <atomic>
#include <inspectable.h>
#include <mutex>
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
// IVector<T>, in the order of windows.foundation.collections.h
struct IObjectVector : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned index, IInspectable **item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned *size) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetView(IObjectVectorView **view) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *item, unsigned *index,
                                              boolean *found) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAt(unsigned index, IInspectable *item) = 0;
    virtual HRESULT STDMETHODCALLTYPE InsertAt(unsigned index, IInspectable *item) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveAt(unsigned index) = 0;
    virtual HRESULT STDMETHODCALLTYPE Append(IInspectable *item) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveAtEnd() = 0;
    virtual HRESULT STDMETHODCALLTYPE Clear() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned start, unsigned capacity,
                                              IInspectable **items, unsigned *actual) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReplaceAll(unsigned count, IInspectable **items) = 0;
};

// Items, each holding a reference, and the IInspectable part of both kinds
template <class Interface> class Items : public Interface {
  public:
    explicit Items(std::vector<IInspectable *> items) : items_(std::move(items)) {}
    virtual ~Items() {
        for (auto *item : items_)
            if (item)
                item->Release();
    }
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        constexpr GUID marshal{0x00000003, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid == marshal)
            return E_NOINTERFACE;
        *object = static_cast<Interface *>(this);
        AddRef();
        return S_OK;
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
    STDMETHODIMP GetRuntimeClassName(HSTRING *name) override {
        *name = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetTrustLevel(TrustLevel *level) override {
        *level = BaseTrust;
        return S_OK;
    }
    STDMETHODIMP GetAt(unsigned index, IInspectable **item) override {
        const std::lock_guard lock(mutex_);
        if (index >= items_.size())
            return E_BOUNDS;
        *item = hold(items_[index]);
        return S_OK;
    }
    STDMETHODIMP get_Size(unsigned *size) override {
        const std::lock_guard lock(mutex_);
        *size = static_cast<unsigned>(items_.size());
        return S_OK;
    }
    STDMETHODIMP IndexOf(IInspectable *item, unsigned *index, boolean *found) override {
        const std::lock_guard lock(mutex_);
        const auto position = std::find(items_.begin(), items_.end(), item);
        *found = position != items_.end();
        *index = *found ? static_cast<unsigned>(position - items_.begin()) : 0;
        return S_OK;
    }
    STDMETHODIMP GetMany(unsigned start, unsigned capacity, IInspectable **items,
                         unsigned *actual) override {
        const std::lock_guard lock(mutex_);
        unsigned count = 0;
        for (; count < capacity && start + count < items_.size(); ++count)
            items[count] = hold(items_[start + count]);
        *actual = count;
        return S_OK;
    }

  protected:
    static IInspectable *hold(IInspectable *item) {
        if (item)
            item->AddRef();
        return item;
    }
    std::mutex mutex_;
    std::vector<IInspectable *> items_;

  private:
    std::atomic<ULONG> references_{1};
};
using VectorView = Items<IObjectVectorView>;

class Vector final : public Items<IObjectVector> {
  public:
    Vector() : Items({}) {}
    STDMETHODIMP GetView(IObjectVectorView **view) override {
        const std::lock_guard lock(mutex_);
        std::vector<IInspectable *> copy;
        for (auto *item : items_)
            copy.push_back(hold(item));
        *view = new VectorView(std::move(copy));
        return S_OK;
    }
    STDMETHODIMP SetAt(unsigned index, IInspectable *item) override {
        const std::lock_guard lock(mutex_);
        if (index >= items_.size())
            return E_BOUNDS;
        if (auto *old = std::exchange(items_[index], hold(item)))
            old->Release();
        return S_OK;
    }
    STDMETHODIMP InsertAt(unsigned index, IInspectable *item) override {
        const std::lock_guard lock(mutex_);
        if (index > items_.size())
            return E_BOUNDS;
        items_.insert(items_.begin() + index, hold(item));
        return S_OK;
    }
    STDMETHODIMP RemoveAt(unsigned index) override {
        const std::lock_guard lock(mutex_);
        if (index >= items_.size())
            return E_BOUNDS;
        if (auto *old = items_[index])
            old->Release();
        items_.erase(items_.begin() + index);
        return S_OK;
    }
    STDMETHODIMP Append(IInspectable *item) override {
        const std::lock_guard lock(mutex_);
        items_.push_back(hold(item));
        return S_OK;
    }
    STDMETHODIMP RemoveAtEnd() override {
        const std::lock_guard lock(mutex_);
        if (items_.empty())
            return E_BOUNDS;
        if (auto *old = items_.back())
            old->Release();
        items_.pop_back();
        return S_OK;
    }
    STDMETHODIMP Clear() override { return ReplaceAll(0, nullptr); }
    STDMETHODIMP ReplaceAll(unsigned count, IInspectable **items) override {
        std::vector<IInspectable *> fresh;
        for (unsigned i = 0; i < count; ++i)
            fresh.push_back(hold(items[i]));
        const std::lock_guard lock(mutex_);
        fresh.swap(items_);
        for (auto *old : fresh)
            if (old)
                old->Release();
        return S_OK;
    }
};
} // namespace

void *make_vector_view(std::vector<IInspectable *> items) {
    return static_cast<IObjectVectorView *>(new VectorView(std::move(items)));
}
void *make_vector() { return static_cast<IObjectVector *>(new Vector); }
} // namespace xs::runtime::durango::winrt
