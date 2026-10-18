#include "runtime/durango/winrt/storage.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/storage.hpp"
#include "runtime/durango/winrt/async.hpp"
#include "runtime/durango/winrt/placeholder.hpp"
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
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>
#include <winstring.h>

namespace xs::runtime::durango::winrt {
namespace {
constexpr std::size_t method_count = 32;
constexpr wchar_t space_class[] = L"Windows.Xbox.Storage.ConnectedStorageSpace";
constexpr wchar_t container_class[] = L"Windows.Xbox.Storage.ConnectedStorageContainer";
constexpr wchar_t query_class[] = L"Windows.Xbox.Storage.ContainerInfoQueryResult";
constexpr wchar_t user_class[] = L"Windows.Xbox.System.User";
// what titles get for a blob or container that is not there
constexpr HRESULT not_found = static_cast<HRESULT>(0x80830008);

constexpr GUID unknown_iid{0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID inspectable_iid{
    0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
constexpr GUID agile_iid{
    0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
constexpr GUID marshal_iid{0x00000003, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID factory_iid{0x00000035, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
// IConnectedStorageSpaceStatics: GetForMachineAsync() is method 8
constexpr GUID space_statics{
    0x07020414, 0xb3e9, 0x47c8, {0xa3, 0x6f, 0xf2, 0x4e, 0x2d, 0xc6, 0x55, 0x73}};
// the other statics interface: method 6 takes the user and gives the user's space
constexpr GUID space_statics2{
    0xdf3fe390, 0x13fa, 0x4814, {0x91, 0xc1, 0xfb, 0x92, 0xf9, 0x0f, 0x64, 0x1d}};
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
        root = std::filesystem::temp_directory_path() / "xscarlet-saves";
        std::fprintf(stderr, "[Runtime] no save folder was given; saves go to %ls\n", root.c_str());
    }
    std::fprintf(stderr, "[Runtime] connected storage: the %s space, in %ls\n", kind,
                 (root / kind).c_str());
    return std::make_shared<SaveSpace>(root / kind);
}

// ContainerInfo2 as titles read it back from the vector view. The field order is that of
// the metadata of the desktop GameSaveContainerInfo, which was made from it: a guess.
struct ContainerInfo2 {
    HSTRING name;
    std::uint64_t total_size;
    HSTRING display_name;
    std::int64_t last_modified; // Windows::Foundation::DateTime
    boolean needs_sync;
};
static_assert(sizeof(ContainerInfo2) == 40);

// IVectorView<ContainerInfo2>. Every interface asked for is taken to be the view, like
// make_vector_view does.
struct IInfoView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned index, ContainerInfo2 *item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned *size) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(void *item, unsigned *index, boolean *found) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(unsigned start, unsigned capacity,
                                              ContainerInfo2 *items, unsigned *actual) = 0;
};
class InfoView final : public IInfoView {
  public:
    explicit InfoView(std::vector<SaveSpace::Container> items) : items_(std::move(items)) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid == marshal_iid)
            return E_NOINTERFACE;
        *object = static_cast<IInfoView *>(this);
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
    STDMETHODIMP GetAt(unsigned index, ContainerInfo2 *item) override {
        if (index >= items_.size())
            return static_cast<HRESULT>(0x8000000B); // E_BOUNDS
        fill(items_[index], *item);
        return S_OK;
    }
    STDMETHODIMP get_Size(unsigned *size) override {
        *size = static_cast<unsigned>(items_.size());
        return S_OK;
    }
    STDMETHODIMP IndexOf(void *, unsigned *index, boolean *found) override {
        *index = 0;
        *found = false;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetMany(unsigned start, unsigned capacity, ContainerInfo2 *items,
                         unsigned *actual) override {
        unsigned count = 0;
        for (; count < capacity && start + count < items_.size(); ++count)
            fill(items_[start + count], items[count]);
        *actual = count;
        return S_OK;
    }

  private:
    static void fill(const SaveSpace::Container &from, ContainerInfo2 &to) {
        // DateTime counts 100 ns from 1601, the system clock from 1970
        constexpr std::int64_t from_1601 = 116444736000000000;
        const auto since_1970 = std::chrono::duration_cast<
            std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>>(
            from.modified.time_since_epoch());
        to = {hstring(from.name), from.size, hstring(from.display_name),
              since_1970.count() + from_1601, false};
    }
    std::atomic<ULONG> references_{1};
    std::vector<SaveSpace::Container> items_;
};

// The objects of connected storage. Each one answers QueryInterface for the interfaces we
// know with another facet sharing its state, and for the rest with a placeholder. The
// methods we know are in call(); the others report themselves, like a placeholder's.
enum class Kind { statics, space, container, query };
struct State {
    Kind kind;
    std::shared_ptr<SaveSpace> space;
    std::string name; // of the container, or the prefix of the query
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
        break;
    }
    return query_class;
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
        break;
    }
    return false;
}
std::string text(const GUID &iid) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2],
                  iid.Data4[3], iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
    return buffer;
}

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

std::optional<HRESULT> call(const Object &self, std::size_t method,
                            const std::array<void *, 11> &a) {
    const auto &state = *self.state;
    const auto out = [&](std::size_t index, void *value) {
        *static_cast<void **>(a[index]) = value;
        return S_OK;
    };
    const bool direct = self.iid == inspectable_iid;
    switch (state.kind) {
    case Kind::statics:
        if (self.iid == space_statics && method == 8) // GetForMachineAsync()
            return out(0, space_operation("machine"));
        if (self.iid == space_statics2 && method == 6) // (User, IAsyncOperation **)
            return out(1, space_operation("user"));
        break;
    case Kind::space:
        if (direct && method == 6) // the user
            return out(0, shared_placeholder(user_class));
        if (direct && (method == 9 || method == 11)) { // CreateContainer, CreateContainerInfoQuery
            const auto name = utf8(static_cast<HSTRING>(a[0]));
            return out(1, make_object(std::make_shared<State>(State{
                              method == 9 ? Kind::container : Kind::query, state.space, name})));
        }
        break;
    case Kind::container:
        if (direct && method == 8)
            return out(2, submit(state, static_cast<IInspectable *>(a[0]),
                                 static_cast<IInspectable *>(a[1]), {}));
        if (direct && method == 9)
            return out(1, read(state, static_cast<IInspectable *>(a[0])));
        if (self.iid == container2 && method == 6)
            return out(3,
                       submit(state, static_cast<IInspectable *>(a[0]),
                              static_cast<IInspectable *>(a[1]), utf8(static_cast<HSTRING>(a[2]))));
        break;
    case Kind::query:
        if (self.iid == query2 && method == 7) { // GetContainerInfo2Async()
            std::vector<SaveSpace::Container> found;
            try {
                found = state.space->containers(state.name);
            } catch (const std::exception &e) {
                std::fprintf(stderr, "[Runtime] could not list containers: %s\n", e.what());
            }
            return out(0, make_completed_operation(
                              static_cast<IInfoView *>(new InfoView(std::move(found)))));
        }
        break;
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
} // namespace xs::runtime::durango::winrt
