#include "runtime/modules/dstorage.hpp"
#include "runtime/common/runtime.hpp"
#include <catch2/catch.hpp>
#include <windows.h>

#include <unknwn.h>

// DirectStorage as a title reaches it: DStorageGetFactory with the IID of the factory of the
// kit it was built with, then the factory's slots.
namespace {
using namespace xv::runtime;

constexpr GUID older_factory{
    0xfb3b54ac, 0xbf82, 0x463a, {0xb7, 0x77, 0x73, 0x19, 0x58, 0x37, 0x86, 0x4d}};
constexpr GUID newer_factory{
    0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr GUID older_queue{
    0xa3f6649a, 0x8c2c, 0x4b3a, {0x8b, 0x6e, 0x10, 0x18, 0x6a, 0x04, 0xb9, 0xbc}};
constexpr GUID newer_queue{
    0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};
constexpr GUID unknown_iid{
    0x01234567, 0x89ab, 0xcdef, {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};

struct QueueDesc {
    UINT64 source_type;
    UINT16 capacity;
    INT8 priority;
    const char *name;
};
struct NewerQueueDesc {
    QueueDesc desc;
    UINT64 unknown;
};

HRESULT get_factory(const GUID &iid, void **factory) {
    static const auto function = [] {
        static Registry registry;
        modules::install_dstorage(registry);
        using Function = HRESULT WINAPI(const GUID *, void **);
        return reinterpret_cast<Function *>(registry.find("xcrdapi.dll", "DStorageGetFactory"));
    }();
    REQUIRE(function);
    return function(&iid, factory);
}
template <class... Arguments> HRESULT call(void *object, int slot, Arguments... arguments) {
    using Function = HRESULT(STDMETHODCALLTYPE *)(void *, Arguments...);
    return reinterpret_cast<Function>((*static_cast<void ***>(object))[slot])(object, arguments...);
}
} // namespace

TEST_CASE("the DirectStorage factory answers the IIDs of the older and the newer kit") {
    void *older = nullptr, *newer = nullptr, *none = &older;
    REQUIRE(get_factory(older_factory, &older) == S_OK);
    REQUIRE(get_factory(newer_factory, &newer) == S_OK);
    CHECK(older == newer);
    CHECK(get_factory(unknown_iid, &none) == E_NOINTERFACE);
    CHECK(none == nullptr);
    static_cast<IUnknown *>(older)->Release();
    static_cast<IUnknown *>(newer)->Release();
}

TEST_CASE("an older title still makes its queue in slot 3") {
    void *factory = nullptr, *queue = nullptr, *none = &factory;
    REQUIRE(get_factory(older_factory, &factory) == S_OK);
    const QueueDesc desc{0, 64, 0, "older"};
    REQUIRE(call(factory, 3, &desc, &older_queue, &queue) == S_OK);
    REQUIRE(queue);
    CHECK(call(factory, 3, &desc, &unknown_iid, &none) == E_NOINTERFACE);
    static_cast<IUnknown *>(queue)->Release();
    static_cast<IUnknown *>(factory)->Release();
}

TEST_CASE("the newer factory makes a queue from the longer description in slot 10") {
    void *factory = nullptr, *queue = nullptr, *same = nullptr;
    REQUIRE(get_factory(newer_factory, &factory) == S_OK);
    const NewerQueueDesc desc{{0, 64, 0, "newer"}, 1};
    REQUIRE(call(factory, 10, &desc, &newer_queue, &queue) == S_OK);
    REQUIRE(queue);
    // the queue is the older one with one more IID, so it answers both
    CHECK(static_cast<IUnknown *>(queue)->QueryInterface(older_queue, &same) == S_OK);
    CHECK(same == queue);
    static_cast<IUnknown *>(same)->Release();
    static_cast<IUnknown *>(queue)->Release();
    CHECK(call(factory, 10, static_cast<const NewerQueueDesc *>(nullptr), &newer_queue, &queue) ==
          E_POINTER);
    // slot 9 is not known and says so instead of doing something
    CHECK(call(factory, 9) == E_NOTIMPL);
    static_cast<IUnknown *>(factory)->Release();
}
