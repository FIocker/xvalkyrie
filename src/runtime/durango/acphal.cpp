#include "runtime/common/runtime.hpp"
#include "runtime/durango/d3d11x/layout.hpp"
#include "runtime/durango/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <intrin.h>
#include <mutex>
#include <utility>
#include <vector>
#include <windows.h>

// The audio processor without SHAPE: commands are acknowledged and audio frames ticked so
// the title's mixer runs, nothing is decoded. ABI as FMOD Ex calls it.
namespace xs::runtime::durango {
namespace {
struct ApuRegion {
    std::uintptr_t address;
    std::uint32_t size, apu;
};
std::mutex apu_mutex;
std::vector<ApuRegion> apu_regions;
std::uint32_t next_apu = 0x10000000;
std::uint32_t apu_address(std::uintptr_t address, std::uint32_t size) {
    for (const auto &region : apu_regions)
        if (address - region.address < region.size)
            return region.apu + static_cast<std::uint32_t>(address - region.address);
    const auto apu = next_apu;
    next_apu += (std::max(size, 1u) + 0xfff) & ~0xfffu;
    apu_regions.push_back({address, size, apu});
    return apu;
}
void *apu_memory(std::size_t size, std::uint64_t alignment) {
    auto *const address = allocate_aligned(size, alignment, nullptr);
    if (address) {
        const std::lock_guard lock(apu_mutex);
        apu_address(reinterpret_cast<std::uintptr_t>(address), static_cast<std::uint32_t>(size));
    }
    return address;
}
void release_apu_memory(void *address) {
    {
        const std::lock_guard lock(apu_mutex);
        std::erase_if(apu_regions, [&](const ApuRegion &region) {
            return region.address == reinterpret_cast<std::uintptr_t>(address);
        });
    }
    VirtualFree(address, 0, MEM_RELEASE);
}
HRESULT WINAPI apu_alloc(void **address, std::uint32_t *apu, std::uint32_t size,
                         std::uint32_t alignment, std::uint32_t) {
    if (!address)
        return E_INVALIDARG;
    *address = apu_memory(size, alignment);
    if (!*address)
        return E_OUTOFMEMORY;
    if (apu) {
        const std::lock_guard lock(apu_mutex);
        *apu = apu_address(reinterpret_cast<std::uintptr_t>(*address), size);
    }
    return S_OK;
}
HRESULT WINAPI apu_free(void *address) {
    if (!address)
        return E_INVALIDARG;
    release_apu_memory(address);
    return S_OK;
}
HRESULT WINAPI apu_map_virtual_address(std::uint32_t *apu, void *address, std::uint32_t size) {
    if (!apu)
        return E_INVALIDARG;
    const std::lock_guard lock(apu_mutex);
    *apu = apu_address(reinterpret_cast<std::uintptr_t>(address), size);
    return S_OK;
}
BOOL WINAPI apu_is_virtual_address_valid() { return TRUE; }
HRESULT WINAPI apu_create_heap() { return S_OK; }

// 0x60 bytes; the rest is unknown
struct ShapeContexts {
    std::uint32_t counts[5], reserved;
    void *contexts[5];
};
static_assert(sizeof(ShapeContexts) == 0x40);
constexpr std::size_t context_bytes = 64; // the largest entry seen
std::mutex shape_mutex;
void *shape_arrays[5] = {};
HRESULT WINAPI allocate_shape_contexts(ShapeContexts *contexts) {
    if (!contexts)
        return E_INVALIDARG;
    const std::lock_guard lock(shape_mutex);
    for (int i = 0; i < 5; ++i) {
        contexts->contexts[i] = nullptr;
        if (!contexts->counts[i])
            continue;
        if (!(shape_arrays[i] = apu_memory(contexts->counts[i] * context_bytes, 256)))
            return E_OUTOFMEMORY;
        contexts->contexts[i] = shape_arrays[i];
    }
    std::fprintf(stderr, "[Runtime] SHAPE contexts: %u, %u, %u, %u, %u (none of them run)\n",
                 contexts->counts[0], contexts->counts[1], contexts->counts[2], contexts->counts[3],
                 contexts->counts[4]);
    std::fflush(stderr);
    return S_OK;
}
HRESULT WINAPI release_shape_contexts() {
    const std::lock_guard lock(shape_mutex);
    for (auto *&array : shape_arrays)
        if (array)
            release_apu_memory(std::exchange(array, nullptr));
    return S_OK;
}

struct Message {
    std::uint32_t type, reserved;
    std::uint64_t value, id;
};
static_assert(sizeof(Message) == 24);
constexpr std::uint32_t audio_frame = 2, command_done = 0x10, disconnected = 0x80;
constexpr auto frame = std::chrono::microseconds(2667);

struct Hal;
struct HalVtable {
    HRESULT(STDMETHODCALLTYPE *connect)(Hal *, std::uint32_t, std::uint32_t);
    HRESULT(STDMETHODCALLTYPE *disconnect)(Hal *);
    HRESULT(STDMETHODCALLTYPE *submit)(Hal *, std::uint32_t, std::uint64_t, std::uint32_t, void *,
                                       std::uint32_t);
    bool(STDMETHODCALLTYPE *pop)(Hal *, Message *);
    std::uint32_t(STDMETHODCALLTYPE *message_count)(Hal *);
    void(STDMETHODCALLTYPE *release)(Hal *);
};
struct Hal {
    const HalVtable *vtable;
    std::mutex mutex;
    std::deque<Message> messages;
    bool connected = false;
    std::chrono::steady_clock::time_point next_frame;
    std::uint32_t seen[8] = {}; // command types logged
};
HRESULT STDMETHODCALLTYPE connect(Hal *self, std::uint32_t commands, std::uint32_t messages) {
    const std::lock_guard lock(self->mutex);
    self->connected = true;
    self->next_frame = std::chrono::steady_clock::now() + frame;
    std::fprintf(stderr, "[Runtime] audio processor connected (%u commands, %u messages)\n",
                 commands, messages);
    std::fflush(stderr);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE disconnect(Hal *self) {
    const std::lock_guard lock(self->mutex);
    self->connected = false;
    self->messages.push_back({disconnected, 0, 0, 0});
    return S_OK;
}
HRESULT STDMETHODCALLTYPE submit(Hal *self, std::uint32_t type, std::uint64_t id,
                                 std::uint32_t frame_index, void *data, std::uint32_t) {
    const std::lock_guard lock(self->mutex);
    if (!self->connected)
        return E_UNEXPECTED;
    if (type < 256 && !(self->seen[type / 32] & 1u << type % 32)) {
        self->seen[type / 32] |= 1u << type % 32;
        std::fprintf(stderr, "[Runtime] audio processor command 0x%x\n", type);
        std::fflush(stderr);
    }
    if (static std::atomic<int> shown{0}; std::getenv("XS_ACP_TRACE") && shown++ < 40) {
        const auto *words = static_cast<const std::uint32_t *>(data);
        std::fprintf(stderr,
                     "[Runtime] audio command %u id %llx frame %u data %x %x %x %x from %s\n",
                     type, static_cast<unsigned long long>(id), frame_index, words ? words[0] : 0,
                     words ? words[1] : 0, words ? words[2] : 0, words ? words[3] : 0,
                     detail::guest_location(_ReturnAddress()).c_str());
    }
    if (id) { // 0: nobody waits for it
        // the first development kits have the command's id right after its type, with no
        // time before them
        Message done{command_done, 0, 0, id};
        if (d3d11x::first_development_kits())
            done.value = std::uint64_t{static_cast<std::uint32_t>(id)} << 32 | type;
        self->messages.push_back(done);
    }
    return S_OK;
}
bool STDMETHODCALLTYPE pop(Hal *self, Message *message) {
    if (!message)
        return false;
    if (static std::atomic<int> shown{0}; std::getenv("XS_ACP_TRACE") && shown++ < 1)
        std::fprintf(stderr, "[Runtime] PopMessage from %s\n",
                     detail::guest_location(_ReturnAddress()).c_str());
    const std::lock_guard lock(self->mutex);
    if (self->messages.empty() && self->connected) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= self->next_frame) {
            self->next_frame = std::max(self->next_frame + frame, now - 8 * frame);
            *message = {audio_frame, 0, 0, 0};
            return true;
        }
    }
    if (self->messages.empty())
        return false;
    *message = self->messages.front();
    self->messages.pop_front();
    return true;
}
// GetNumMessages: what PopMessage has to give, the audio frame that is due among them
std::uint32_t STDMETHODCALLTYPE message_count(Hal *self) {
    const std::lock_guard lock(self->mutex);
    const bool frame_due = self->connected && std::chrono::steady_clock::now() >= self->next_frame;
    return static_cast<std::uint32_t>(self->messages.size()) + frame_due;
}
void STDMETHODCALLTYPE release(Hal *self) { delete self; }
constexpr HalVtable hal_vtable{&connect, &disconnect, &submit, &pop, &message_count, &release};

HRESULT WINAPI acp_hal_create(Hal **hal) {
    if (!hal)
        return E_INVALIDARG;
    *hal = new Hal{&hal_vtable};
    return S_OK;
}

template <class F> void *erase(F *function) { return reinterpret_cast<void *>(function); }
} // namespace

void install_acphal(Registry &registry) {
    const std::pair<const char *, void *> functions[] = {
        {"ApuAlloc", erase(apu_alloc)},
        {"ApuFree", erase(apu_free)},
        {"ApuCreateHeap", erase(apu_create_heap)},
        {"ApuMapVirtualAddress", erase(apu_map_virtual_address)},
        {"ApuIsVirtualAddressValid", erase(apu_is_virtual_address_valid)},
        {"AcpHalCreate", erase(acp_hal_create)},
        {"AcpHalAllocateShapeContexts", erase(allocate_shape_contexts)},
        {"AcpHalReleaseShapeContexts", erase(release_shape_contexts)}};
    for (const auto &[name, function] : functions)
        registry.add("acphal.dll", name, function);
}
} // namespace xs::runtime::durango
