#include "runtime/durango/winrt/storage.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/storage.hpp"
#include "runtime/durango/winrt/async.hpp"
#include "runtime/durango/winrt/placeholder.hpp"
#include "runtime/durango/winrt/title_info.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <inspectable.h>
#include <intrin.h>
#include <map>
#include <memory>
#include <optional>
#include <roapi.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>
#include <winstring.h>

namespace xv::runtime::durango::winrt {
namespace {
using modules::SaveSpace;
constexpr std::size_t method_count = 32;
constexpr wchar_t space_class[] = L"Windows.Xbox.Storage.ConnectedStorageSpace";
constexpr wchar_t container_class[] = L"Windows.Xbox.Storage.ConnectedStorageContainer";
constexpr wchar_t query_class[] = L"Windows.Xbox.Storage.ContainerInfoQueryResult";
constexpr wchar_t blob_query_class[] = L"Windows.Xbox.Storage.BlobInfoQueryResult";
constexpr wchar_t user_class[] = L"Windows.Xbox.System.User";
// what titles get for a blob or container that is not there
constexpr HRESULT not_found = static_cast<HRESULT>(0x80830008);
#ifndef E_BOUNDS
#define E_BOUNDS static_cast<HRESULT>(0x8000000B)
#endif

constexpr GUID unknown_iid{0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID inspectable_iid{
    0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
constexpr GUID agile_iid{
    0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
constexpr GUID marshal_iid{0x00000003, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID factory_iid{0x00000035, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
// IConnectedStorageSpaceStatics: GetForUserAsync(User) is method 6, GetForMachineAsync() 8
constexpr GUID space_statics{
    0x07020414, 0xb3e9, 0x47c8, {0xa3, 0x6f, 0xf2, 0x4e, 0x2d, 0xc6, 0x55, 0x73}};
// the other statics interface: method 6 takes the user and gives the user's space
constexpr GUID space_statics2{
    0xdf3fe390, 0x13fa, 0x4814, {0x91, 0xc1, 0xfb, 0x92, 0xf9, 0x0f, 0x64, 0x1d}};
// IConnectedStorageSpace2: GetRemainingBytesInQuota64Async
constexpr GUID space2{0xbc5b91a7, 0x2afc, 0x46fd, {0x81, 0xd4, 0x74, 0xb8, 0x18, 0x8d, 0x74, 0x8a}};
// IConnectedStorageContainer2: SubmitUpdatesAsync with a display name is method 6
constexpr GUID container2{
    0xaf964b72, 0xec65, 0x46de, {0x93, 0x5c, 0x84, 0x5a, 0xd0, 0x19, 0x4f, 0x0d}};
// IContainerInfoQueryResult2: GetContainerInfo2Async() is method 7
constexpr GUID query2{0xf8a8cdfe, 0x1d10, 0x42a5, {0xb0, 0x1e, 0x41, 0x71, 0x8e, 0x18, 0x3c, 0xa9}};
// IIterable<IKeyValuePair<String, IBuffer>>, which a title's IMapView<String, IBuffer> is too
constexpr GUID blob_map_iterable{
    0x3c9ffa92, 0x5123, 0x5ac4, {0xb1, 0x11, 0x03, 0xc2, 0x15, 0xf0, 0xc5, 0x1c}};
constexpr GUID buffer_byte_access{
    0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};

// The collection and buffer interfaces as titles hand them over. Their IIDs depend on the
// type arguments, so only the vtable is spelled out.
struct IIterableRaw : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE First(IInspectable **iterator) = 0;
};
struct IIteratorRaw : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Current(void *current) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *has) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveNext(boolean *has) = 0;
};
struct IKeyValuePairRaw : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Key(HSTRING *key) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Value(IInspectable **value) = 0;
};
struct IBufferRaw : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Capacity(UINT32 *capacity) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Length(UINT32 *length) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Length(UINT32 length) = 0;
};
struct IBufferByteAccessRaw : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE **bytes) = 0;
};

template <class T> struct Ref {
    T *pointer = nullptr;
    Ref() = default;
    Ref(const Ref &) = delete;
    Ref &operator=(const Ref &) = delete;
    Ref(Ref &&other) noexcept : pointer(std::exchange(other.pointer, nullptr)) {}
    ~Ref() {
        if (pointer)
            pointer->Release();
    }
    T *operator->() const { return pointer; }
    void **out() { return reinterpret_cast<void **>(&pointer); }
};
struct String {
    HSTRING handle = nullptr;
    ~String() { WindowsDeleteString(handle); }
};

std::string utf8(HSTRING text) {
    UINT32 length = 0;
    const auto *const wide = WindowsGetStringRawBuffer(text, &length);
    if (!length)
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), nullptr, 0,
                                         nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), result.data(), size, nullptr,
                        nullptr);
    return result;
}
HSTRING hstring(const std::string &text) {
    std::wstring wide(text.size(), L'\0');
    wide.resize(static_cast<std::size_t>(
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                            static_cast<int>(wide.size()))));
    HSTRING result = nullptr;
    WindowsCreateString(wide.c_str(), static_cast<UINT32>(wide.size()), &result);
    return result;
}
void check(HRESULT result) {
    if (FAILED(result)) {
        char text[64];
        std::snprintf(text, sizeof text, "a collection of the title failed with 0x%08lx",
                      static_cast<unsigned long>(result));
        throw std::runtime_error(text);
    }
}

// What an IMapView<String, IBuffer> of the title holds, buffers with a reference each.
std::vector<std::pair<std::string, Ref<IBufferRaw>>> blob_map(IInspectable *map) {
    std::vector<std::pair<std::string, Ref<IBufferRaw>>> result;
    if (!map)
        return result;
    Ref<IIterableRaw> iterable;
    check(map->QueryInterface(blob_map_iterable, iterable.out()));
    Ref<IIteratorRaw> iterator;
    check(iterable->First(reinterpret_cast<IInspectable **>(iterator.out())));
    boolean has = false;
    for (check(iterator->get_HasCurrent(&has)); has; check(iterator->MoveNext(&has))) {
        Ref<IKeyValuePairRaw> pair;
        check(iterator->get_Current(pair.out()));
        String key;
        check(pair->get_Key(&key.handle));
        Ref<IBufferRaw> buffer;
        check(pair->get_Value(reinterpret_cast<IInspectable **>(buffer.out())));
        result.emplace_back(utf8(key.handle), std::move(buffer));
    }
    return result;
}
// the strings of an IIterable<String> of the title, which may be null
std::vector<std::string> names(IInspectable *list) {
    std::vector<std::string> result;
    if (!list)
        return result;
    Ref<IIteratorRaw> iterator;
    check(static_cast<IIterableRaw *>(list)->First(
        reinterpret_cast<IInspectable **>(iterator.out())));
    boolean has = false;
    for (check(iterator->get_HasCurrent(&has)); has; check(iterator->MoveNext(&has))) {
        String name;
        check(iterator->get_Current(&name.handle));
        result.push_back(utf8(name.handle));
    }
    return result;
}
BYTE *bytes_of(IBufferRaw *buffer) {
    Ref<IBufferByteAccessRaw> access;
    check(buffer->QueryInterface(buffer_byte_access, access.out()));
    BYTE *bytes = nullptr;
    check(access->Buffer(&bytes));
    return bytes;
}

std::shared_ptr<SaveSpace> open_space(const char *kind) {
    auto root = save_directory();
    if (root.empty()) {
        root = std::filesystem::temp_directory_path() / "xvalkyrie-saves";
        std::fprintf(stderr, "[Runtime] no save folder was given; saves go to %ls\n", root.c_str());
    }
    std::fprintf(stderr, "[Runtime] connected storage: the %s space, in %ls\n", kind,
                 (root / kind).c_str());
    return std::make_shared<SaveSpace>(root / kind);
}

// Bits every object here has: COM identity for one interface, and a count.
template <class Interface> class Counted : public Interface {
  public:
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
    virtual ~Counted() = default;

  private:
    std::atomic<ULONG> references_{1};
};

// The three structs of the queries, as the metadata has them (the interface IDs computed
// from these signatures are the ones titles ask for):
//   ContainerInfo  {String Name; UInt32 TotalSize}
//   ContainerInfo2 {String Name; UInt64 TotalSize; String DisplayName; DateTime
//                   LastModifiedTime; Boolean NeedsSync}
//   BlobInfo       {String Name; UInt32 Size}
enum class Shape { container, container2, blob };
struct ContainerInfo {
    HSTRING name;
    std::uint32_t total_size;
};
struct ContainerInfo2 {
    HSTRING name;
    std::uint64_t total_size;
    HSTRING display_name;
    std::int64_t last_modified;
    boolean needs_sync;
};
static_assert(sizeof(ContainerInfo) == 16 && sizeof(ContainerInfo2) == 40);
struct Record {
    std::string name, display_name;
    std::uint64_t size = 0;
    std::chrono::system_clock::time_point modified;
};
constexpr GUID view_iids[] = {
    {0x6074aba5, 0x5f5d, 0x5595, {0xb7, 0x09, 0xa1, 0x3b, 0x89, 0xec, 0xf8, 0x46}},
    {0xe8cc68f3, 0x4c1f, 0x5f92, {0xb0, 0xb6, 0x0a, 0x43, 0xc2, 0x47, 0x19, 0x73}},
    {0x51b469a2, 0x6c7f, 0x5109, {0x8e, 0x13, 0xac, 0x20, 0x9d, 0x44, 0xac, 0x29}}};
constexpr GUID iterable_iids[] = {
    {0xf0e21fa2, 0xf44a, 0x54d2, {0xbe, 0xe2, 0x0e, 0x84, 0xf1, 0x01, 0xc8, 0xe3}},
    {0x53f0bb98, 0x9aef, 0x5d69, {0x9c, 0xf4, 0xba, 0x72, 0x2a, 0xf7, 0x80, 0xde}},
    {0x14cc6c4b, 0x2594, 0x55a4, {0xb0, 0xae, 0x78, 0x1d, 0x25, 0x3a, 0xf0, 0x9f}}};
std::size_t stride(Shape shape) {
    return shape == Shape::container2 ? sizeof(ContainerInfo2) : sizeof(ContainerInfo);
}
// writes the struct of `shape` for `from` at `to`; the strings are the caller's to free
void fill(Shape shape, const Record &from, void *to) {
    if (shape != Shape::container2) {
        const ContainerInfo info{hstring(from.name), static_cast<std::uint32_t>(from.size)};
        std::memcpy(to, &info, sizeof info);
        return;
    }
    // DateTime counts 100 ns from 1601, the system clock from 1970
    constexpr std::int64_t from_1601 = 116444736000000000;
    const auto since_1970 =
        std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>>(
            from.modified.time_since_epoch());
    const ContainerInfo2 info{hstring(from.name), from.size, hstring(from.display_name),
                              since_1970.count() + from_1601, false};
    std::memcpy(to, &info, sizeof info);
}

// IVectorView<T> and IIterable<T> of one of the structs; IIterator<T> over it.
struct IStructView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned index, void *item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned *size) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(void *item, unsigned *index, boolean *found) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned start, unsigned capacity, void *items,
                                              unsigned *actual) = 0;
};
struct IStructIterator : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Current(void *item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *has) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveNext(boolean *has) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned capacity, void *items, unsigned *actual) = 0;
};
struct Records {
    Shape shape;
    std::vector<Record> items;
};
class StructIterator final : public Counted<IStructIterator> {
  public:
    explicit StructIterator(std::shared_ptr<const Records> records)
        : records_(std::move(records)) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        *object = iid == marshal_iid ? nullptr : static_cast<IStructIterator *>(this);
        if (!*object)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP get_Current(void *item) override {
        if (at_ >= records_->items.size())
            return E_BOUNDS;
        fill(records_->shape, records_->items[at_], item);
        return S_OK;
    }
    STDMETHODIMP get_HasCurrent(boolean *has) override {
        *has = at_ < records_->items.size();
        return S_OK;
    }
    STDMETHODIMP MoveNext(boolean *has) override {
        if (at_ < records_->items.size())
            ++at_;
        return get_HasCurrent(has);
    }
    STDMETHODIMP GetMany(unsigned capacity, void *items, unsigned *actual) override {
        unsigned count = 0;
        for (; count < capacity && at_ < records_->items.size(); ++count, ++at_)
            fill(records_->shape, records_->items[at_],
                 static_cast<char *>(items) + count * stride(records_->shape));
        *actual = count;
        return S_OK;
    }

  private:
    std::shared_ptr<const Records> records_;
    std::size_t at_ = 0;
};
class StructView final : public Counted<IStructView>, public IIterableRaw {
  public:
    explicit StructView(Records records)
        : records_(std::make_shared<const Records>(std::move(records))) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        const auto shape = static_cast<std::size_t>(records_->shape);
        if (iid == marshal_iid)
            return E_NOINTERFACE;
        if (iid == iterable_iids[shape])
            *object = static_cast<IIterableRaw *>(this);
        else // the view itself, under whatever name the title has for it
            *object = static_cast<IStructView *>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return Counted::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return Counted::Release(); }
    STDMETHODIMP GetIids(ULONG *count, IID **iids) override {
        return Counted::GetIids(count, iids);
    }
    STDMETHODIMP GetRuntimeClassName(HSTRING *name) override {
        return Counted::GetRuntimeClassName(name);
    }
    STDMETHODIMP GetTrustLevel(TrustLevel *level) override { return Counted::GetTrustLevel(level); }
    STDMETHODIMP GetAt(unsigned index, void *item) override {
        if (index >= records_->items.size())
            return E_BOUNDS;
        fill(records_->shape, records_->items[index], item);
        return S_OK;
    }
    STDMETHODIMP get_Size(unsigned *size) override {
        *size = static_cast<unsigned>(records_->items.size());
        return S_OK;
    }
    STDMETHODIMP IndexOf(void *, unsigned *index, boolean *found) override {
        *index = 0;
        *found = false;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetMany(unsigned start, unsigned capacity, void *items,
                         unsigned *actual) override {
        unsigned count = 0;
        for (; count < capacity && start + count < records_->items.size(); ++count)
            fill(records_->shape, records_->items[start + count],
                 static_cast<char *>(items) + count * stride(records_->shape));
        *actual = count;
        return S_OK;
    }
    STDMETHODIMP First(IInspectable **iterator) override {
        *iterator = new StructIterator(records_);
        return S_OK;
    }

  private:
    std::shared_ptr<const Records> records_;
};

// IMapView<String, IBuffer> of blobs read by GetAsync, with buffers of the host's.
constexpr GUID blob_map_view{
    0x2cfeec4f, 0xe261, 0x5f4c, {0xae, 0xe1, 0xc7, 0x85, 0x18, 0xe9, 0xd5, 0xb9}};
struct IBlobMapView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Lookup(HSTRING key, IInspectable **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned *size) = 0;
    virtual HRESULT STDMETHODCALLTYPE HasKey(HSTRING key, boolean *found) = 0;
    virtual HRESULT STDMETHODCALLTYPE Split(IInspectable **first, IInspectable **second) = 0;
};
struct IPairIterator : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Current(IInspectable **pair) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean *has) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveNext(boolean *has) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned capacity, IInspectable **items,
                                              unsigned *actual) = 0;
};
using Blobs = std::map<std::string, Ref<IBufferRaw>>;
class Pair final : public Counted<IKeyValuePairRaw> {
  public:
    Pair(std::string key, IBufferRaw *value) : key_(std::move(key)), value_(value) {
        value_->AddRef();
    }
    ~Pair() override { value_->Release(); }
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        *object = iid == marshal_iid ? nullptr : static_cast<IKeyValuePairRaw *>(this);
        if (!*object)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP get_Key(HSTRING *key) override {
        *key = hstring(key_);
        return S_OK;
    }
    STDMETHODIMP get_Value(IInspectable **value) override {
        value_->AddRef();
        *value = value_;
        return S_OK;
    }

  private:
    std::string key_;
    IBufferRaw *value_;
};
class PairIterator final : public Counted<IPairIterator> {
  public:
    explicit PairIterator(std::shared_ptr<const Blobs> blobs)
        : blobs_(std::move(blobs)), at_(blobs_->begin()) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        *object = iid == marshal_iid ? nullptr : static_cast<IPairIterator *>(this);
        if (!*object)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP get_Current(IInspectable **pair) override {
        if (at_ == blobs_->end())
            return E_BOUNDS;
        *pair = new Pair(at_->first, at_->second.pointer);
        return S_OK;
    }
    STDMETHODIMP get_HasCurrent(boolean *has) override {
        *has = at_ != blobs_->end();
        return S_OK;
    }
    STDMETHODIMP MoveNext(boolean *has) override {
        if (at_ != blobs_->end())
            ++at_;
        return get_HasCurrent(has);
    }
    STDMETHODIMP GetMany(unsigned capacity, IInspectable **items, unsigned *actual) override {
        unsigned count = 0;
        for (; count < capacity && at_ != blobs_->end(); ++count, ++at_)
            items[count] = new Pair(at_->first, at_->second.pointer);
        *actual = count;
        return S_OK;
    }

  private:
    std::shared_ptr<const Blobs> blobs_;
    Blobs::const_iterator at_;
};
class BlobMap final : public Counted<IBlobMapView>, public IIterableRaw {
  public:
    explicit BlobMap(Blobs blobs) : blobs_(std::make_shared<const Blobs>(std::move(blobs))) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid == marshal_iid)
            return E_NOINTERFACE;
        if (iid == blob_map_iterable)
            *object = static_cast<IIterableRaw *>(this);
        else
            *object = static_cast<IBlobMapView *>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return Counted::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return Counted::Release(); }
    STDMETHODIMP GetIids(ULONG *count, IID **iids) override {
        return Counted::GetIids(count, iids);
    }
    STDMETHODIMP GetRuntimeClassName(HSTRING *name) override {
        return Counted::GetRuntimeClassName(name);
    }
    STDMETHODIMP GetTrustLevel(TrustLevel *level) override { return Counted::GetTrustLevel(level); }
    STDMETHODIMP Lookup(HSTRING key, IInspectable **value) override {
        const auto found = blobs_->find(utf8(key));
        if (found == blobs_->end())
            return E_BOUNDS;
        found->second->AddRef();
        *value = found->second.pointer;
        return S_OK;
    }
    STDMETHODIMP get_Size(unsigned *size) override {
        *size = static_cast<unsigned>(blobs_->size());
        return S_OK;
    }
    STDMETHODIMP HasKey(HSTRING key, boolean *found) override {
        *found = blobs_->contains(utf8(key));
        return S_OK;
    }
    STDMETHODIMP Split(IInspectable **first, IInspectable **second) override {
        *first = *second = nullptr;
        return S_OK;
    }
    STDMETHODIMP First(IInspectable **iterator) override {
        *iterator = new PairIterator(blobs_);
        return S_OK;
    }

  private:
    std::shared_ptr<const Blobs> blobs_;
};
// a Windows.Storage.Streams.Buffer of the host holding `bytes`
Ref<IBufferRaw> host_buffer(const std::vector<std::uint8_t> &bytes) {
    struct IBufferFactoryRaw : IInspectable {
        virtual HRESULT STDMETHODCALLTYPE Create(UINT32 capacity, IBufferRaw **buffer) = 0;
    };
    static constexpr GUID buffer_factory{
        0x71af914d, 0xc10f, 0x484b, {0xbc, 0x50, 0x14, 0xbc, 0x62, 0x3b, 0x3a, 0x27}};
    static const auto factory = [] {
        constexpr wchar_t name[] = L"Windows.Storage.Streams.Buffer";
        HSTRING_HEADER header;
        HSTRING text = nullptr;
        IBufferFactoryRaw *result = nullptr;
        if (FAILED(WindowsCreateStringReference(name, static_cast<UINT32>(wcslen(name)), &header,
                                                &text)) ||
            FAILED(
                RoGetActivationFactory(text, buffer_factory, reinterpret_cast<void **>(&result))))
            throw std::runtime_error("no Windows.Storage.Streams.Buffer on the host");
        return result;
    }();
    Ref<IBufferRaw> buffer;
    check(factory->Create(static_cast<UINT32>(bytes.size()), &buffer.pointer));
    if (!bytes.empty())
        std::memcpy(bytes_of(buffer.pointer), bytes.data(), bytes.size());
    check(buffer->put_Length(static_cast<UINT32>(bytes.size())));
    return buffer;
}

// The objects of connected storage. Each one answers QueryInterface for the interfaces we
// know with another facet sharing its state, and for the rest with a placeholder. The
// methods we know are in call(); the others report themselves, like a placeholder's.
enum class Kind { statics, space, container, query, blob_query };
struct State {
    Kind kind;
    std::shared_ptr<SaveSpace> space;
    std::string name;           // of the container, or the prefix of the query
    std::string container = {}; // of a blob query
};
struct Object {
    void *const *vtable;
    std::atomic<ULONG> references{1};
    GUID iid;
    std::shared_ptr<State> state;
};
void *make_object(std::shared_ptr<State> state, const GUID &iid = inspectable_iid);

const wchar_t *class_of(Kind kind) {
    switch (kind) {
    case Kind::statics:
    case Kind::space:
        return space_class;
    case Kind::container:
        return container_class;
    case Kind::query:
        return query_class;
    case Kind::blob_query:
        break;
    }
    return blob_query_class;
}
bool facet(Kind kind, const GUID &iid) {
    switch (kind) {
    case Kind::statics:
        return iid == factory_iid || iid == space_statics || iid == space_statics2;
    case Kind::container:
        return iid == container2;
    case Kind::query:
        return iid == query2;
    case Kind::space:
        return iid == space2;
    case Kind::blob_query:
        break;
    }
    return false;
}
// the default interface, which titles mostly use without asking for it
bool default_interface(Kind kind, const GUID &iid) {
    static constexpr GUID defaults[] = {
        inspectable_iid,
        {0xb48eea29, 0x8323, 0x41bc, {0xbb, 0xad, 0x2c, 0x26, 0xe2, 0x7b, 0xda, 0x22}},
        {0x885c65fe, 0xe884, 0x4cff, {0xba, 0x58, 0xfb, 0x82, 0xcc, 0x32, 0xb5, 0x52}},
        {0x4f937563, 0xbae3, 0x4062, {0xa6, 0x7f, 0xcb, 0xea, 0x61, 0xa2, 0xed, 0xc3}},
        {0x5598379c, 0x82de, 0x4952, {0xaa, 0x30, 0x49, 0xae, 0x32, 0x9c, 0x58, 0x85}}};
    return iid == inspectable_iid || iid == defaults[static_cast<std::size_t>(kind)];
}
std::string text(const GUID &iid) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2],
                  iid.Data4[3], iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
    return buffer;
}

// the containers (or the blobs of `state.container`) whose name starts with the prefix
Records records(const State &state, Shape shape) {
    Records result{shape, {}};
    try {
        if (shape == Shape::blob)
            for (const auto &blob : state.space->blobs(state.container, state.name))
                result.items.push_back({blob.name, {}, blob.size, {}});
        else
            for (const auto &container : state.space->containers(state.name))
                result.items.push_back(
                    {container.name, container.display_name, container.size, container.modified});
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[Runtime] could not list connected storage: %s\n", e.what());
    }
    return result;
}
// GetContainerInfoAsync, GetContainerInfo2Async, GetBlobInfoAsync: all of them, or `count`
// from `start`
void *query_operation(const State &state, Shape shape, std::size_t start = 0,
                      std::size_t count = SIZE_MAX) {
    auto found = records(state, shape);
    auto &items = found.items;
    items.erase(items.begin(), items.begin() + std::min(start, items.size()));
    if (items.size() > count)
        items.resize(count);
    return make_completed_operation(static_cast<IStructView *>(new StructView(std::move(found))));
}
// GetAsync(IIterable<String>, IAsyncOperation<IMapView<String, IBuffer>> **)
void *get(const State &container, IInspectable *wanted) {
    try {
        Blobs blobs;
        for (const auto &name : names(wanted)) {
            const auto data = container.space->read(container.name, name);
            if (!data) {
                std::fprintf(stderr, "[Runtime] no blob \"%s\" in container \"%s\"\n", name.c_str(),
                             container.name.c_str());
                return make_failed_operation(not_found);
            }
            blobs.emplace(name, host_buffer(*data));
        }
        return make_completed_operation(static_cast<IBlobMapView *>(new BlobMap(std::move(blobs))));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[Runtime] could not read container \"%s\": %s\n",
                     container.name.c_str(), e.what());
        return make_failed_operation(E_FAIL);
    }
}
// what titles are told is left of a quota nothing enforces
constexpr unsigned long long remaining_bytes = 256ull << 20;

void *space_operation(const char *kind) {
    return make_completed_operation(
        make_object(std::make_shared<State>(State{Kind::space, open_space(kind), {}})));
}
// SubmitUpdatesAsync(IMapView<String, IBuffer>, IIterable<String>[, String], IAsyncAction **)
void *submit(const State &container, IInspectable *writes, IInspectable *deletes,
             std::optional<std::string> display_name) {
    try {
        std::map<std::string, std::vector<std::uint8_t>> blobs;
        for (const auto &[name, buffer] : blob_map(writes)) {
            UINT32 length = 0;
            check(buffer->get_Length(&length));
            const auto *const bytes = length ? bytes_of(buffer.pointer) : nullptr;
            blobs[name].assign(bytes, bytes + length);
        }
        const auto deleted = names(deletes);
        container.space->submit(container.name, blobs, deleted, display_name);
        std::fprintf(stderr, "[Runtime] saved container \"%s\": %zu blobs written, %zu deleted\n",
                     container.name.c_str(), blobs.size(), deleted.size());
        return make_completed_action();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[Runtime] could not save container \"%s\": %s\n",
                     container.name.c_str(), e.what());
        return make_failed_action(E_FAIL);
    }
}
// ReadAsync(IMapView<String, IBuffer>, IAsyncAction **): fills the title's buffers
void *read(const State &container, IInspectable *reads) {
    try {
        for (const auto &[name, buffer] : blob_map(reads)) {
            const auto data = container.space->read(container.name, name);
            if (!data) {
                std::fprintf(stderr, "[Runtime] no blob \"%s\" in container \"%s\"\n", name.c_str(),
                             container.name.c_str());
                return make_failed_action(not_found);
            }
            UINT32 capacity = 0;
            check(buffer->get_Capacity(&capacity));
            if (data->size() > capacity) {
                std::fprintf(stderr,
                             "[Runtime] blob \"%s\" of container \"%s\" has %zu bytes, the "
                             "title's buffer %u\n",
                             name.c_str(), container.name.c_str(), data->size(), capacity);
                return make_failed_action(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
            }
            if (!data->empty())
                std::memcpy(bytes_of(buffer.pointer), data->data(), data->size());
            check(buffer->put_Length(static_cast<UINT32>(data->size())));
        }
        return make_completed_action();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[Runtime] could not read container \"%s\": %s\n",
                     container.name.c_str(), e.what());
        return make_failed_action(E_FAIL);
    }
}

// Method numbers are those of the metadata: from 6, in the order the interface declares them.
std::optional<HRESULT> call(const Object &self, std::size_t method,
                            const std::array<void *, 11> &a) {
    const auto &state = *self.state;
    const auto out = [&](std::size_t index, void *value) {
        *static_cast<void **>(a[index]) = value;
        return S_OK;
    };
    const auto string = [&](std::size_t index, const std::string &text) {
        *static_cast<HSTRING *>(a[index]) = hstring(text);
        return S_OK;
    };
    const auto number = [&](std::size_t index) {
        return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(a[index]) & 0xffffffff);
    };
    const auto make = [&](Kind kind, std::string name, std::string container = {}) {
        return make_object(std::make_shared<State>(
            State{kind, state.space, std::move(name), std::move(container)}));
    };
    const bool direct = default_interface(state.kind, self.iid);
    switch (state.kind) {
    case Kind::statics:
        // GetForUserAsync(User[, String]), GetForMachineAsync([String]): where the result goes
        if (self.iid == space_statics && method >= 6 && method <= 9) {
            constexpr std::size_t result[] = {1, 2, 0, 1};
            return out(result[method - 6], space_operation(method < 8 ? "user" : "machine"));
        }
        // GetSyncOnDemandForUserAsync(User[, String]): everything is here already
        if (self.iid == space_statics2 && (method == 6 || method == 7))
            return out(method - 5, space_operation("user"));
        break;
    case Kind::space:
        if (self.iid == space2 && method == 6) // GetRemainingBytesInQuota64Async
            return out(0, make_completed_value(remaining_bytes, 8));
        if (!direct)
            break;
        switch (method) {
        case 6: // User
            return out(0, shared_placeholder(user_class));
        case 7: // ServiceConfigurationId
            *static_cast<HSTRING *>(a[0]) = nullptr;
            {
                const auto id = title_info(L"service_config_id");
                return WindowsCreateString(id.c_str(), static_cast<UINT32>(id.size()),
                                           static_cast<HSTRING *>(a[0]));
            }
        case 8: // IsReadOnly
            *static_cast<boolean *>(a[0]) = false;
            return S_OK;
        case 9: // CreateContainer(String)
            return out(1, make(Kind::container, utf8(static_cast<HSTRING>(a[0]))));
        case 10: { // DeleteContainerAsync(String)
            const auto name = utf8(static_cast<HSTRING>(a[0]));
            try {
                const bool removed = state.space->remove(name);
                std::fprintf(stderr, "[Runtime] deleted container \"%s\"%s\n", name.c_str(),
                             removed ? "" : " (there was none)");
                return out(1, removed ? make_completed_action() : make_failed_action(not_found));
            } catch (const std::exception &e) {
                std::fprintf(stderr, "[Runtime] %s\n", e.what());
                return out(1, make_failed_action(E_FAIL));
            }
        }
        case 11: // CreateContainerInfoQuery(String prefix)
            return out(1, make(Kind::query, utf8(static_cast<HSTRING>(a[0]))));
        case 12: // GetRemainingBytesInQuotaAsync
            return out(0, make_completed_value(remaining_bytes, 4));
        }
        break;
    case Kind::container:
        if (self.iid == container2) {
            if (method == 6) // SubmitUpdatesAsync(blobs, deletes, display name)
                return out(3, submit(state, static_cast<IInspectable *>(a[0]),
                                     static_cast<IInspectable *>(a[1]),
                                     utf8(static_cast<HSTRING>(a[2]))));
            if (method == 8) // CreateBlobInfoQuery(String prefix)
                return out(1, make(Kind::blob_query, utf8(static_cast<HSTRING>(a[0])), state.name));
            break;
        }
        if (!direct)
            break;
        switch (method) {
        case 6: // Name
            return string(0, state.name);
        case 7: // OwningSpace
            return out(0, make(Kind::space, {}));
        case 8: // SubmitUpdatesAsync(blobs, deletes)
            return out(2, submit(state, static_cast<IInspectable *>(a[0]),
                                 static_cast<IInspectable *>(a[1]), {}));
        case 9: // ReadAsync(blobs)
            return out(1, read(state, static_cast<IInspectable *>(a[0])));
        case 10: // GetAsync(names)
            return out(1, get(state, static_cast<IInspectable *>(a[0])));
        }
        break;
    case Kind::query:
    case Kind::blob_query: {
        const auto shape = state.kind == Kind::blob_query ? Shape::blob
                           : self.iid == query2           ? Shape::container2
                                                          : Shape::container;
        if (!direct && self.iid != query2)
            break;
        if (method == 6) // (UInt32 start, UInt32 count, IAsyncOperation **)
            return out(2, query_operation(state, shape, number(0), number(1)));
        if (method == 7) // (IAsyncOperation **)
            return out(0, query_operation(state, shape));
        if (method == 8 && shape != Shape::container2) // GetItemCountAsync
            return out(0, make_completed_value(records(state, shape).items.size(), 4));
        break;
    }
    }
    return std::nullopt;
}

HRESULT STDMETHODCALLTYPE query(Object *self, const GUID &iid, void **object) {
    if (!object)
        return E_POINTER;
    *object = nullptr;
    if (iid == unknown_iid || iid == inspectable_iid || iid == agile_iid || iid == self->iid) {
        ++self->references;
        *object = self;
        return S_OK;
    }
    if (iid == marshal_iid)
        return E_NOINTERFACE;
    if (facet(self->state->kind, iid)) {
        *object = make_object(self->state, iid);
        return S_OK;
    }
    return placeholder_interface(class_of(self->state->kind), iid, object);
}
ULONG STDMETHODCALLTYPE add_ref(Object *self) { return ++self->references; }
ULONG STDMETHODCALLTYPE release(Object *self) {
    const auto left = --self->references;
    if (!left && self->state->kind != Kind::statics)
        delete self;
    return left;
}
HRESULT STDMETHODCALLTYPE get_iids(Object *, ULONG *count, GUID **iids) {
    *count = 0;
    *iids = nullptr;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE get_class_name(Object *self, HSTRING *name) {
    const auto *const text = class_of(self->state->kind);
    return WindowsCreateString(text, static_cast<UINT32>(wcslen(text)), name);
}
HRESULT STDMETHODCALLTYPE get_trust_level(Object *, int *level) {
    *level = 0;
    return S_OK;
}
template <std::size_t Index>
HRESULT STDMETHODCALLTYPE method(Object *self, void *a1, void *a2, void *a3, void *a4, void *a5,
                                 void *a6, void *a7, void *a8, void *a9, void *a10, void *a11) {
    if (const auto result = call(*self, Index, {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11}))
        return *result;
    return unknown_method(class_of(self->state->kind), text(self->iid).c_str(), Index, a1, a2,
                          _ReturnAddress());
}
template <std::size_t... Index>
constexpr std::array<void *, method_count> make_vtable(std::index_sequence<Index...>) {
    return {reinterpret_cast<void *>(&query),
            reinterpret_cast<void *>(&add_ref),
            reinterpret_cast<void *>(&release),
            reinterpret_cast<void *>(&get_iids),
            reinterpret_cast<void *>(&get_class_name),
            reinterpret_cast<void *>(&get_trust_level),
            reinterpret_cast<void *>(&method<Index + 6>)...};
}
const std::array<void *, method_count> vtable =
    make_vtable(std::make_index_sequence<method_count - 6>{});

void *make_object(std::shared_ptr<State> state, const GUID &iid) {
    return new Object{vtable.data(), 1, iid, std::move(state)};
}
} // namespace

IInspectable *storage_class(std::wstring_view name) {
    if (name != space_class)
        return nullptr;
    static auto *const statics = static_cast<IInspectable *>(
        make_object(std::make_shared<State>(State{Kind::statics, nullptr, {}})));
    return statics;
}
} // namespace xv::runtime::durango::winrt
