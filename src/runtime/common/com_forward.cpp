#include "runtime/common/com_forward.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <windows.h>

namespace xs::runtime {
struct Forwarder {
    void *const *vtable;
    std::atomic<ULONG> references;
    IUnknown *host; // the thunks read this at offset 16
    ForwarderQuery query;
    std::unique_ptr<void *[]> methods;
};
namespace {
constexpr std::size_t slot_limit = 256, thunk_size = 16;
std::mutex mutex;
std::set<const Forwarder *> live;

// One thunk per slot, shared by every forwarder:
//   mov rcx, [rcx+16]
//   mov rax, [rcx]
//   jmp [rax + slot*8]
const std::uint8_t *thunks() {
    static const std::uint8_t *const code = [] {
        auto *memory = static_cast<std::uint8_t *>(VirtualAlloc(
            nullptr, slot_limit * thunk_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!memory)
            throw std::runtime_error("Cannot allocate COM forwarding thunks");
        for (std::size_t slot = 0; slot < slot_limit; ++slot) {
            auto *thunk = memory + slot * thunk_size;
            const std::uint8_t head[] = {0x48, 0x8b, 0x49, 0x10, 0x48, 0x8b, 0x01, 0xff, 0xa0};
            std::memcpy(thunk, head, sizeof head);
            const auto offset = static_cast<std::uint32_t>(slot * 8);
            std::memcpy(thunk + sizeof head, &offset, 4);
        }
        DWORD previous;
        if (!VirtualProtect(memory, slot_limit * thunk_size, PAGE_EXECUTE_READ, &previous))
            throw std::runtime_error("Cannot protect COM forwarding thunks");
        return memory;
    }();
    return code;
}
HRESULT STDMETHODCALLTYPE query(Forwarder *self, REFIID iid, void **object) {
    return self->query(self, iid, object);
}
ULONG STDMETHODCALLTYPE release(Forwarder *self) {
    const auto left = --self->references;
    if (!left) {
        {
            const std::lock_guard lock(mutex);
            live.erase(self);
        }
        self->host->Release();
        delete self;
    }
    return left;
}
} // namespace

Forwarder *make_forwarder(IUnknown *host, std::size_t slots, ForwarderQuery query_interface,
                          std::initializer_list<SlotOverride> overrides) {
    if (!host || slots < 3 || slots > slot_limit)
        throw std::invalid_argument("Bad COM forwarder");
    auto methods = std::make_unique<void *[]>(slots);
    for (std::size_t slot = 3; slot < slots; ++slot)
        methods[slot] = const_cast<std::uint8_t *>(thunks() + slot * thunk_size);
    methods[0] = reinterpret_cast<void *>(&query);
    methods[1] = reinterpret_cast<void *>(&forwarder_add_ref);
    methods[2] = reinterpret_cast<void *>(&release);
    for (const auto &entry : overrides)
        methods[entry.slot] = entry.function;
    auto *const self = new Forwarder{methods.get(), 1, host, query_interface, std::move(methods)};
    const std::lock_guard lock(mutex);
    live.insert(self);
    return self;
}
IUnknown *forwarder_host(Forwarder *self) { return self->host; }
ULONG forwarder_add_ref(Forwarder *self) { return ++self->references; }
IUnknown *unwrap_forwarder(IUnknown *object) {
    const std::lock_guard lock(mutex);
    const auto *candidate = reinterpret_cast<const Forwarder *>(object);
    return live.contains(candidate) ? candidate->host : object;
}
} // namespace xs::runtime
