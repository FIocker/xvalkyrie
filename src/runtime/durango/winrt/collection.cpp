#include "runtime/durango/winrt/collection.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <inspectable.h>
#include <mutex>
#include <utility>
#include <windows.h>

namespace xv::runtime::durango::winrt {
namespace {
// GetAt takes the whole register: see Items::GetAt
struct IObjectVectorView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(std::uint64_t index, IInspectable **item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned *size) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(IInspectable *item, unsigned *index,
                                              boolean *found) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned start, unsigned capacity,
                                              IInspectable **items, unsigned *actual) = 0;
};
// IVector<T>, in the order of windows.foundation.collections.h
struct IObjectVector : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(std::uint64_t index, IInspectable **item) = 0;
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

IInspectable *make_iterator(const std::vector<IInspectable *> &items);
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
    // Slot 6 is GetAt for IVectorView<T> and IVector<T>, and First for IIterable<T>, which
    // the collection is as well: those interface ids depend on T, so every id gets the same
    // object. An index is 32 bits, the register's upper half zero; First's argument is a
    // pointer, where it is not.
    STDMETHODIMP GetAt(std::uint64_t index, IInspectable **item) override {
        const std::lock_guard lock(mutex_);
        if (index >> 32) {
            *reinterpret_cast<IInspectable **>(index) = make_iterator(items_);
            return S_OK;
        }
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
// IIterator<T>, in the order of windows.foundation.collections.h, over a copy of the items
class Iterator final : public IInspectable {
  public:
    explicit Iterator(std::vector<IInspectable *> items) : items_(std::move(items)) {
        for (auto *item : items_)
            if (item)
                item->AddRef();
    }
    ~Iterator() {
        for (auto *item : items_)
            if (item)
                item->Release();
    }
    STDMETHODIMP QueryInterface(REFIID, void **object) override {
        if (!object)
            return E_POINTER;
        *object = this;
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
    virtual HRESULT STDMETHODCALLTYPE get_Current(IInspectable **item) {
        if (at_ >= items_.size())
            return E_BOUNDS;
        *item = items_[at_];
        if (*item)
            (*item)->AddRef();
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *value) {
        *value = at_ < items_.size();
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE MoveNext(boolean *value) {
        if (at_ < items_.size())
            ++at_;
        *value = at_ < items_.size();
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned capacity, IInspectable **items,
                                              unsigned *actual) {
        unsigned count = 0;
        for (; count < capacity && at_ < items_.size(); ++count, ++at_) {
            items[count] = items_[at_];
            if (items[count])
                items[count]->AddRef();
        }
        *actual = count;
        return S_OK;
    }

  private:
    std::vector<IInspectable *> items_;
    std::size_t at_ = 0;
    std::atomic<ULONG> references_{1};
};
IInspectable *make_iterator(const std::vector<IInspectable *> &items) {
    return new Iterator(items);
}
} // namespace

void *make_vector_view(std::vector<IInspectable *> items) {
    return static_cast<IObjectVectorView *>(new VectorView(std::move(items)));
}
void *make_vector() { return static_cast<IObjectVector *>(new Vector); }
} // namespace xv::runtime::durango::winrt
