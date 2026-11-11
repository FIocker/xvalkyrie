#include "runtime/modules/d3d12x.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/d3d12x/internal.hpp"
#include "runtime/modules/placement.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <intrin.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <wrl/implements.h>

// d3d12_x.dll and d3d12_xs.dll on the host's Direct3D 12. Both consoles have the same
// interfaces up to the methods each added last; `console` says which the title was built for.
namespace xs::runtime::modules::d3d12x {
namespace {
Target console = Target::durango;
AdapterWrapper adapter_wrapper = nullptr;
template <class... Interfaces> bool is_any(REFIID iid) {
    return ((iid == __uuidof(Interfaces)) || ...);
}
// The title's first direct queue: the console presents for the device, the host for a queue
std::atomic<ID3D12CommandQueue *> first_direct_queue{nullptr};
// d3d12_x.dll: root signatures are the host's format as well; the device is not there yet
template <class Function> Function host_d3d12(const char *name) {
    static const auto library = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return library ? reinterpret_cast<Function>(GetProcAddress(library, name)) : nullptr;
}
HRESULT WINAPI serialize_root_signature(const void *desc, UINT version, void **blob,
                                        void **errors) {
    using Serialize = HRESULT(WINAPI *)(const void *, UINT, void **, void **);
    static const auto serialize = host_d3d12<Serialize>("D3D12SerializeRootSignature");
    return serialize ? serialize(desc, version, blob, errors) : E_NOTIMPL;
}
HRESULT WINAPI serialize_versioned_root_signature(const void *desc, void **blob, void **errors) {
    using Serialize = HRESULT(WINAPI *)(const void *, void **, void **);
    static const auto serialize = host_d3d12<Serialize>("D3D12SerializeVersionedRootSignature");
    return serialize ? serialize(desc, blob, errors) : E_NOTIMPL;
}
HRESULT WINAPI root_signature_deserializer(const void *blob, SIZE_T size, REFIID iid,
                                           void **reader) {
    using Create = HRESULT(WINAPI *)(const void *, SIZE_T, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateRootSignatureDeserializer");
    return create ? create(blob, size, iid, reader) : E_NOTIMPL;
}
HRESULT WINAPI versioned_root_signature_deserializer(const void *blob, SIZE_T size, REFIID iid,
                                                     void **reader) {
    using Create = HRESULT(WINAPI *)(const void *, SIZE_T, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateVersionedRootSignatureDeserializer");
    return create ? create(blob, size, iid, reader) : E_NOTIMPL;
}
HRESULT WINAPI create_device_12(void *adapter, UINT level, REFIID iid, void **device) {
    if (device)
        *device = nullptr;
    std::fprintf(stderr, "[Runtime] D3D12CreateDevice(%p, 0x%x): Direct3D 12.X is not there yet\n",
                 adapter, level);
    std::fflush(stderr);
    return E_NOTIMPL;
}
// The console's Direct3D 12 device answers for IDXGIDevice1, which the host's does not: this
// one leads to the adapter the device was made on, and from there to our factory.
class D3D12DxgiDevice final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDXGIDevice1> {
  public:
    explicit D3D12DxgiDevice(LUID luid) : luid_(luid) {}
    STDMETHODIMP GetAdapter(IDXGIAdapter **adapter) override {
        if (!adapter)
            return E_POINTER;
        *adapter = nullptr;
        IDXGIFactory4 *factory = nullptr;
        auto result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (FAILED(result))
            return result;
        IDXGIAdapter *host = nullptr;
        result = factory->EnumAdapterByLuid(luid_, IID_PPV_ARGS(&host));
        factory->Release();
        if (FAILED(result) || !adapter_wrapper) {
            *adapter = host;
            return result;
        }
        return adapter_wrapper(host, adapter);
    }
    STDMETHODIMP GetParent(REFIID iid, void **parent) override {
        if (!is_any<IDXGIAdapter, IDXGIAdapter1, IDXGIAdapter2, IDXGIAdapter3, IDXGIAdapter4>(iid))
            return E_NOINTERFACE;
        return GetAdapter(reinterpret_cast<IDXGIAdapter **>(parent));
    }
    STDMETHODIMP SetPrivateData(REFGUID, UINT, const void *) override { return S_OK; }
    STDMETHODIMP SetPrivateDataInterface(REFGUID, const IUnknown *) override { return S_OK; }
    STDMETHODIMP GetPrivateData(REFGUID, UINT *, void *) override { return DXGI_ERROR_NOT_FOUND; }
    STDMETHODIMP CreateSurface(const DXGI_SURFACE_DESC *, UINT, DXGI_USAGE,
                               const DXGI_SHARED_RESOURCE *, IDXGISurface **) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP QueryResourceResidency(IUnknown *const *, DXGI_RESIDENCY *, UINT) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP SetGPUThreadPriority(INT) override { return S_OK; }
    STDMETHODIMP GetGPUThreadPriority(INT *priority) override {
        *priority = 0;
        return S_OK;
    }
    STDMETHODIMP SetMaximumFrameLatency(UINT) override { return S_OK; }
    STDMETHODIMP GetMaximumFrameLatency(UINT *latency) override {
        *latency = 3;
        return S_OK;
    }

  private:
    LUID luid_;
};
// The console's Direct3D 12 objects are the host's with a vtable of their own, one slot
// before which is the host's: being the host's objects, they go back to the host's API as
// they are, whichever way the title got hold of them.
using Slot = void *;
Slot *original_vtable(void *object) {
    return static_cast<Slot *>((*static_cast<Slot **>(object))[-1]);
}
template <class Method> Method original(void *object, std::size_t index) {
    return reinterpret_cast<Method>(original_vtable(object)[index]);
}
// The host's runtime calls its own objects through their vtable too: a slot the console has
// differently runs the console's method only when the title calls it, and the host's
// otherwise (its callers pass the object first; the title's, for methods that return a
// structure, second). The console's methods are one table per kind of object.
enum class Family { device, list, heap, queue, resource };
constexpr std::size_t max_slots = 160;
template <Family Kind> std::array<Slot, max_slots> console_table{};
using Raw = std::uint64_t(STDMETHODCALLTYPE *)(void *, std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t);
// Whether the call that returns to `address` is the title's own call of this slot: the
// instruction before it is `call [reg + Index * 8]`. The host's runtime also gets here with
// the title's return address, when one of its methods jumps on to another slot.
bool title_called_slot(const void *address, std::size_t index) {
    if (!detail::in_title(address))
        return false;
    const auto *code = static_cast<const std::uint8_t *>(address);
    const auto offset = static_cast<std::uint32_t>(index * 8);
    std::uint32_t disp32;
    std::memcpy(&disp32, code - 4, 4);
    // FF /2 with a 32-bit displacement: FF 90+r, or FF 94 and a SIB byte
    if (disp32 == offset &&
        ((code[-6] == 0xff && (code[-5] & 0xf8) == 0x90) || (code[-7] == 0xff && code[-6] == 0x94)))
        return true;
    // slot 0, no displacement: FF 10+r
    if (offset == 0 && code[-2] == 0xff && (code[-1] & 0xf8) == 0x10)
        return true;
    // with an 8-bit displacement: FF 50+r d8
    if (offset < 0x80 && code[-1] == offset && code[-3] == 0xff && (code[-2] & 0xf8) == 0x50)
        return true;
    // `call reg` (FF D0+r, with a prefix for r8 to r15): the slot was loaded into the register
    // first, with `mov reg, [reg + Index * 8]` a few instructions back. Titles built against
    // the GDK's header call this way.
    if (code[-2] == 0xff && (code[-1] & 0xf8) == 0xd0) {
        for (const auto *at = code - 3; at > code - 160; --at) {
            if (at[0] != 0x8b)
                continue;
            const auto mode = at[1] >> 6;
            const std::size_t skip = (at[1] & 7) == 4 ? 3 : 2; // a SIB byte follows
            std::uint32_t loaded = ~0u;
            if (mode == 2)
                std::memcpy(&loaded, at + skip, 4);
            else if (mode == 1)
                loaded = at[skip];
            else if (mode == 0)
                loaded = 0;
            if (loaded == offset && (mode != 0 || offset == 0))
                return true;
        }
        return false;
    }
    // Another slot's call is the host's runtime going on from one method to the next. A call
    // of a function of the title's own is the title's: the function ends in a jump through
    // the slot (the out-of-line side of the device's inline methods does).
    if ((code[-6] == 0xff && (code[-5] & 0xf8) == 0x90) || (code[-7] == 0xff && code[-6] == 0x94) ||
        (code[-3] == 0xff && (code[-2] & 0xf8) == 0x50) ||
        (code[-2] == 0xff && (code[-1] & 0xf8) == 0x10))
        return false;
    std::int32_t relative;
    std::memcpy(&relative, code - 4, 4);
    return code[-5] == 0xe8 && detail::in_title(code + relative);
}
template <Family Kind, std::size_t Index>
std::uint64_t STDMETHODCALLTYPE by_caller(void *first, std::uint64_t a, std::uint64_t b,
                                          std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                          std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                          std::uint64_t i, std::uint64_t j) {
    const auto method = title_called_slot(_ReturnAddress(), Index) ? console_table<Kind>[Index]
                                                                   : original_vtable(first)[Index];
    return reinterpret_cast<Raw>(method)(first, a, b, c, d, e, f, g, h, i, j);
}
template <Family Kind, std::size_t... Index>
std::array<Slot, max_slots> by_caller_slots(std::index_sequence<Index...>) {
    return {reinterpret_cast<Slot>(&by_caller<Kind, Index>)...};
}
template <Family Kind>
const auto dispatchers = by_caller_slots<Kind>(std::make_index_sequence<max_slots>{});
// One console vtable per host vtable (one per class of the host's runtime); an object that
// has one already keeps it
template <Family Kind>
void console_vtable(void *object, std::size_t host_slots, std::size_t slots,
                    const std::vector<std::pair<std::size_t, Slot>> &overrides) {
    static std::mutex mutex;
    static std::map<Slot *, Slot *> blocks; // host vtable -> console vtable
    const std::lock_guard lock(mutex);
    auto *const host = *static_cast<Slot **>(object);
    for (const auto &[from, to] : blocks)
        if (to == host)
            return;
    auto &block = blocks[host];
    if (!block) {
        auto *const memory = new Slot[slots + 1]{};
        memory[0] = host;
        std::copy(host, host + host_slots, memory + 1);
        for (const auto &[index, function] : overrides) {
            console_table<Kind>[index] = function;
            memory[1 + index] = dispatchers<Kind>[index];
        }
        block = memory + 1;
    }
    *static_cast<Slot **>(object) = block;
}
// Methods that return an 8-byte structure: the console's return it in RAX, where the
// desktop's C++ methods write it through a hidden pointer
template <std::size_t Index> std::uint64_t STDMETHODCALLTYPE returned_in_rax(void *self) {
    std::uint64_t value = 0;
    original<void *(STDMETHODCALLTYPE *)(void *, std::uint64_t *)>(self, Index)(self, &value);
    return value;
}

// Methods that return a larger structure: the console's take the place for it first and the
// object second, the desktop's C++ methods the other way round
D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
resource_allocation_info(D3D12_RESOURCE_ALLOCATION_INFO *info, ID3D12Device *self, UINT mask,
                         UINT count, const D3D12_RESOURCE_DESC *descs) {
    std::vector<D3D12_RESOURCE_DESC> host(descs, descs + (descs ? count : 0));
    for (auto &desc : host)
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(desc.Flags & 0xff);
    using Get = D3D12_RESOURCE_ALLOCATION_INFO *(
        STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_ALLOCATION_INFO *, UINT, UINT,
                             const D3D12_RESOURCE_DESC *);
    return original<Get>(self, 25)(self, info, mask, count, host.data());
}
D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE custom_heap_properties(D3D12_HEAP_PROPERTIES *properties,
                                                                ID3D12Device *self, UINT node,
                                                                D3D12_HEAP_TYPE type) {
    using Get = D3D12_HEAP_PROPERTIES *(STDMETHODCALLTYPE *)(void *, D3D12_HEAP_PROPERTIES *, UINT,
                                                             D3D12_HEAP_TYPE);
    return original<Get>(self, 26)(self, properties, node, type);
}
// a console method that is a host one at another slot, with the same arguments
template <std::size_t Host>
std::uint64_t STDMETHODCALLTYPE moved_method(void *self, std::uint64_t a, std::uint64_t b,
                                             std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                             std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                             std::uint64_t i, std::uint64_t j) {
    return reinterpret_cast<Raw>(original_vtable(self)[Host])(self, a, b, c, d, e, f, g, h, i, j);
}
// ID3D12Resource as the console has it: the desktop's methods, then GetPlacementDescX and
// GetOpaqueResourceTypeX, which nothing here has
HRESULT STDMETHODCALLTYPE no_console_method(void *) { return E_NOTIMPL; }
void console_resource(void *resource) {
    if (resource && std::getenv("XS_PATCH_RESOURCES"))
        console_vtable<Family::resource>(resource, 15, 17,
                                         {{15, reinterpret_cast<Slot>(&no_console_method)},
                                          {16, reinterpret_cast<Slot>(&no_console_method)}});
}
// ID3D12Device::QueryInterface: the console's device is also its DXGI device
HRESULT STDMETHODCALLTYPE device_query(ID3D12Device *self, REFIID iid, void **object) {
    if (object && is_any<IDXGIObject, IDXGIDevice, IDXGIDevice1>(iid)) {
        *object = static_cast<IDXGIDevice1 *>(
            Microsoft::WRL::Make<D3D12DxgiDevice>(self->GetAdapterLuid()).Detach());
        return S_OK;
    }
    // ID3D12Device8 had another interface ID in the kits than the one the desktop shipped with
    static constexpr GUID kit_device8{
        0x728f1fa2, 0xea11, 0x4a9f, {0x9b, 0x52, 0x9b, 0x42, 0xa4, 0x84, 0xd5, 0xc9}};
    return original<HRESULT(STDMETHODCALLTYPE *)(void *, REFIID, void **)>(self, 0)(
        self, iid == kit_device8 ? __uuidof(ID3D12Device8) : iid, object);
}
// CheckFeatureSupport: some of the console's structures are larger than the desktop's
// (D3D12_OPTIONS is 64 bytes there): the host fills its part, the rest is zero
HRESULT STDMETHODCALLTYPE check_feature_support(ID3D12Device *self, D3D12_FEATURE feature,
                                                void *data, UINT size) {
    using Check = HRESULT(STDMETHODCALLTYPE *)(void *, D3D12_FEATURE, void *, UINT);
    const auto check = original<Check>(self, 13);
    auto result = check(self, feature, data, size);
    if (result != E_INVALIDARG || !data)
        return result;
    std::vector<std::uint8_t> input(static_cast<std::uint8_t *>(data),
                                    static_cast<std::uint8_t *>(data) + size);
    for (UINT smaller = size - 4; smaller >= 4 && smaller + 32 >= size; smaller -= 4) {
        std::memcpy(data, input.data(), size);
        if (SUCCEEDED(result = check(self, feature, data, smaller))) {
            std::memset(static_cast<std::uint8_t *>(data) + smaller, 0, size - smaller);
            break;
        }
    }
    return result;
}
// Descriptor heaps as the console has them: a CPU handle is the address of the descriptor,
// below 4 GiB, and titles write descriptors there themselves. Each heap gets such memory of
// its own, a descriptor's worth per host descriptor; the host's descriptor at the same index
// is the one that counts. Descriptors made through the device are the host's already (the
// shadow holds a mark); those the title wrote are hardware descriptors, read when used.
struct ShadowHeap {
    std::uintptr_t base, end;
    D3D12_CPU_DESCRIPTOR_HANDLE host;
    UINT stride;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    ID3D12Device *device;
    bool shader_visible;
    std::uint64_t gpu; // the host's GPU handle of the start, for a shader-visible heap
    UINT host_stride;
    void *owner; // the heap the title holds
};
// The console's descriptor sizes, which titles also have compiled in: the device's inline
// methods copy descriptors of these sizes themselves
UINT console_stride(D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? 32
           : type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV       ? 112
           : type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV       ? 128
                                                          : 16;
}
UINT STDMETHODCALLTYPE descriptor_increment(ID3D12Device *, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return console_stride(type);
}
std::mutex shadow_mutex;
std::map<std::uintptr_t, ShadowHeap> shadow_heaps;         // by base
std::map<void *, std::uintptr_t> shadow_of;                // heap -> base
constexpr std::uint64_t host_mark = 0x5853'4844'4553'4352; // "XSHDESCR"
void *low_memory(std::size_t bytes) {
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.HighestEndingAddress = reinterpret_cast<void *>(0xffff'ffffull);
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    return VirtualAlloc2(nullptr, nullptr, (bytes + 0xffff) & ~std::size_t{0xffff},
                         MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE, &parameter, 1);
}
const ShadowHeap *shadow_heap(std::uintptr_t address) {
    auto found = shadow_heaps.upper_bound(address);
    if (found == shadow_heaps.begin())
        return nullptr;
    --found;
    return address < found->second.end ? &found->second : nullptr;
}
// the host's handle for a console one; others are the host's already
D3D12_CPU_DESCRIPTOR_HANDLE host_handle(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    const std::lock_guard lock(shadow_mutex);
    if (const auto *heap = shadow_heap(handle.ptr))
        return {heap->host.ptr + (handle.ptr - heap->base) / heap->stride * heap->host_stride};
    return handle;
}
D3D12_CPU_DESCRIPTOR_HANDLE host_slot(const ShadowHeap &heap, std::uintptr_t at) {
    return {heap.host.ptr + (at - heap.base) / heap.stride * heap.host_stride};
}
// The mark of a host descriptor, and the host's handle of the slot that holds it: a title
// that copies descriptors itself (the device's inline CopyDescriptorsSimple) carries both
// along, which tells where the host's descriptor of the copy is to come from.
void set_mark(const ShadowHeap &heap, std::uintptr_t at) {
    at = heap.base + (at - heap.base) / heap.stride * heap.stride;
    const std::uint64_t words[2]{host_mark, host_slot(heap, at).ptr};
    std::memcpy(reinterpret_cast<void *>(at), words, sizeof words);
}
void mark_host(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    const std::lock_guard lock(shadow_mutex);
    if (const auto *heap = shadow_heap(handle.ptr))
        set_mark(*heap, handle.ptr);
}
// A marked descriptor that names another slot is a copy the title made itself: the host's
// descriptor follows it
void resolve(const ShadowHeap &heap, std::uintptr_t at) {
    at = heap.base + (at - heap.base) / heap.stride * heap.stride;
    std::uint64_t words[2];
    std::memcpy(words, reinterpret_cast<const void *>(at), sizeof words);
    const auto own = host_slot(heap, at);
    if (words[0] != host_mark || words[1] == own.ptr || words[1] == 0)
        return;
    using Copy = void(STDMETHODCALLTYPE *)(void *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                           D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
    original<Copy>(heap.device, 24)(heap.device, 1, own, D3D12_CPU_DESCRIPTOR_HANDLE{words[1]},
                                    heap.type);
    set_mark(heap, at);
}
// the host's handle for a console one whose descriptor is about to be read
D3D12_CPU_DESCRIPTOR_HANDLE host_view(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    ShadowHeap heap;
    {
        const std::lock_guard lock(shadow_mutex);
        const auto *found = shadow_heap(handle.ptr);
        if (!found)
            return handle;
        heap = *found;
    }
    resolve(heap, handle.ptr);
    return host_slot(heap, handle.ptr);
}
// a constant buffer view of nothing, as the device's inline CreateConstantBufferView writes it
bool null_buffer(const std::uint32_t *words) { return words[2] == 1 && words[3] == 0x0002403c; }
// A descriptor the title wrote: made on the host from what the hardware descriptor says.
// Buffers (V#: 48-bit address, stride in bits 48-61, record count in the third dword) become
// constant buffer views.
void decode_written(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    const ShadowHeap *heap;
    {
        const std::lock_guard lock(shadow_mutex);
        heap = shadow_heap(handle.ptr);
    }
    if (!heap || heap->type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        return;
    std::uint32_t words[4];
    std::memcpy(words, reinterpret_cast<const void *>(handle.ptr), sizeof words);
    std::uint64_t first;
    std::memcpy(&first, words, sizeof first);
    if (first == host_mark)
        resolve(*heap, handle.ptr);
    if (first == host_mark || first == 0 || null_buffer(words))
        return;
    // what kinds of descriptors titles write, by the type in the last dword (buffers 0, images
    // 8 and up), once each
    if (static std::atomic<std::uint32_t> seen{0}; std::getenv("XS_D3D_TRACE")) {
        const std::uint32_t bit = 1u << (words[3] >> 28);
        if (!(seen.fetch_or(bit) & bit))
            std::fprintf(stderr, "[Runtime] descriptor written by the title: %08x %08x %08x %08x\n",
                         words[0], words[1], words[2], words[3]);
    }
    const std::uint64_t address = first & 0xffff'ffff'ffffull;
    const std::uint32_t stride = (words[1] >> 16) & 0x3fff;
    const std::uint64_t size = std::uint64_t{words[2]} * (stride ? stride : 1);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC view{
        address, static_cast<UINT>(std::min<std::uint64_t>((size + 255) & ~255ull, 65536))};
    using Create = void(STDMETHODCALLTYPE *)(void *, const D3D12_CONSTANT_BUFFER_VIEW_DESC *,
                                             D3D12_CPU_DESCRIPTOR_HANDLE);
    original<Create>(heap->device, 17)(heap->device, &view, host_handle(handle));
}
void register_shadow(ID3D12Device *device, void *heap, const D3D12_DESCRIPTOR_HEAP_DESC &desc,
                     D3D12_CPU_DESCRIPTOR_HANDLE host, std::uint64_t gpu) {
    const UINT stride = console_stride(desc.Type);
    auto *const memory = low_memory(std::size_t{desc.NumDescriptors} * stride);
    if (!memory) {
        std::fprintf(stderr, "[Runtime] no memory below 4 GiB for a descriptor heap\n");
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    const std::lock_guard lock(shadow_mutex);
    shadow_heaps[base] = {base,
                          base + std::size_t{desc.NumDescriptors} * stride,
                          host,
                          stride,
                          desc.Type,
                          device,
                          (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0,
                          gpu,
                          device->GetDescriptorHandleIncrementSize(desc.Type),
                          heap};
    shadow_of[heap] = base;
}
// Before the GPU runs: what the title wrote straight into a shader-visible heap since last
// time, read from the pages written to
void decode_written_heaps() {
    std::vector<ShadowHeap> heaps;
    {
        const std::lock_guard lock(shadow_mutex);
        for (const auto &[base, heap] : shadow_heaps)
            if (heap.shader_visible && heap.type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                heaps.push_back(heap);
    }
    std::vector<void *> pages(4096);
    for (const auto &heap : heaps) {
        ULONG_PTR count = pages.size();
        ULONG size = 0;
        const auto bytes = ((heap.end - heap.base) + 0xffff) & ~std::uintptr_t{0xffff};
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void *>(heap.base), bytes,
                          pages.data(), &count, &size) != 0)
            continue;
        for (ULONG_PTR i = 0; i < count; ++i) {
            const auto page = reinterpret_cast<std::uintptr_t>(pages[i]);
            const auto first = std::max(heap.base, page);
            const auto last = std::min(heap.end, page + size);
            for (auto at =
                     heap.base + (first - heap.base + heap.stride - 1) / heap.stride * heap.stride;
                 at + heap.stride <= last; at += heap.stride)
                decode_written({at});
        }
    }
}
// ID3D12DescriptorHeap::GetCPUDescriptorHandleForHeapStart: the shadow's start
// The title's descriptor heap: a proxy of the host's, which the host's runtime keeps as its
// own (it knows its objects by their vtable). The proxy's handles are the console's: the CPU
// one the shadow's start, both in RAX.
struct HeapProxy {
    Slot *vtable;
    ID3D12DescriptorHeap *host;
    std::atomic<ULONG> references;
};
std::mutex heap_proxy_mutex;
std::set<const void *> heap_proxies;
ID3D12DescriptorHeap *host_heap(void *heap) {
    const std::lock_guard lock(heap_proxy_mutex);
    return heap_proxies.contains(heap) ? static_cast<HeapProxy *>(heap)->host
                                       : static_cast<ID3D12DescriptorHeap *>(heap);
}
std::uint64_t STDMETHODCALLTYPE cpu_heap_start(void *self) {
    const std::lock_guard lock(shadow_mutex);
    const auto found = shadow_of.find(self);
    return found != shadow_of.end()
               ? found->second
               : static_cast<HeapProxy *>(self)->host->GetCPUDescriptorHandleForHeapStart().ptr;
}
// the console's GPU handles are its CPU ones: the shadow's start as well
std::uint64_t STDMETHODCALLTYPE gpu_heap_start(void *self) {
    {
        const std::lock_guard lock(shadow_mutex);
        const auto found = shadow_of.find(self);
        if (found != shadow_of.end() && shadow_heaps[found->second].shader_visible)
            return found->second;
    }
    return static_cast<HeapProxy *>(self)->host->GetGPUDescriptorHandleForHeapStart().ptr;
}
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE heap_method(void *self, std::uint64_t a, std::uint64_t b,
                                            std::uint64_t c, std::uint64_t d) {
    auto *const host = static_cast<HeapProxy *>(self)->host;
    return reinterpret_cast<std::uint64_t(STDMETHODCALLTYPE *)(void *, std::uint64_t, std::uint64_t,
                                                               std::uint64_t, std::uint64_t)>(
        (*reinterpret_cast<Slot **>(host))[Index])(host, a, b, c, d);
}
HRESULT STDMETHODCALLTYPE heap_query(void *self, REFIID iid, void **object) {
    if (!object)
        return E_POINTER;
    if (is_any<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12DescriptorHeap>(
            iid)) {
        ++static_cast<HeapProxy *>(self)->references;
        *object = self;
        return S_OK;
    }
    return static_cast<HeapProxy *>(self)->host->QueryInterface(iid, object);
}
ULONG STDMETHODCALLTYPE heap_add_ref(void *self) {
    return ++static_cast<HeapProxy *>(self)->references;
}
ULONG STDMETHODCALLTYPE heap_release(void *self) {
    auto *const proxy = static_cast<HeapProxy *>(self);
    const auto left = --proxy->references;
    if (left == 0) {
        {
            const std::lock_guard lock(heap_proxy_mutex);
            heap_proxies.erase(proxy);
        }
        proxy->host->Release();
        delete proxy;
    }
    return left;
}
template <std::size_t... Index> std::array<Slot, 11> heap_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, 11> vtable{reinterpret_cast<Slot>(&heap_method<Index>)...};
    vtable[0] = reinterpret_cast<Slot>(&heap_query);
    vtable[1] = reinterpret_cast<Slot>(&heap_add_ref);
    vtable[2] = reinterpret_cast<Slot>(&heap_release);
    vtable[9] = reinterpret_cast<Slot>(&cpu_heap_start);
    vtable[10] = reinterpret_cast<Slot>(&gpu_heap_start);
    return vtable;
}
std::array<Slot, 11> heap_vtable = heap_vtable_of(std::make_index_sequence<11>{});
// The buffers the title has, by GPU address: a view of one names the resource, where a
// hardware descriptor names an address
struct BufferRange {
    std::uint64_t end;
    ID3D12Resource *resource;
};
std::mutex buffer_mutex;
std::map<std::uint64_t, BufferRange> buffers_at; // by start
void track_buffer(ID3D12Resource *resource) {
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        return;
    const auto start = resource->GetGPUVirtualAddress();
    const std::lock_guard lock(buffer_mutex);
    buffers_at[start] = {start + desc.Width, resource};
}
bool buffer_at(std::uint64_t address, ID3D12Resource *&resource, std::uint64_t &offset,
               std::uint64_t &size) {
    const std::lock_guard lock(buffer_mutex);
    auto found = buffers_at.upper_bound(address);
    if (found == buffers_at.begin())
        return false;
    --found;
    if (address >= found->second.end)
        return false;
    resource = found->second.resource;
    offset = address - found->first;
    size = found->second.end - address;
    return true;
}

// The descriptor tables of each root signature: the type of the descriptors at each offset
// from a table's start, for the descriptors the title writes itself
struct TableRange {
    D3D12_DESCRIPTOR_RANGE_TYPE type;
    UINT offset, count;
};
std::mutex signature_mutex;
std::map<const void *, std::vector<std::vector<TableRange>>> tables_of; // per parameter
void read_root_signature(const void *signature, const void *blob, SIZE_T size) {
    using Create = HRESULT(WINAPI *)(const void *, SIZE_T, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateVersionedRootSignatureDeserializer");
    ID3D12VersionedRootSignatureDeserializer *reader = nullptr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc = nullptr;
    if (!create || FAILED(create(blob, size, IID_PPV_ARGS(&reader))) ||
        FAILED(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc))) {
        if (reader)
            reader->Release();
        return;
    }
    std::vector<std::vector<TableRange>> tables(desc->Desc_1_1.NumParameters);
    for (UINT i = 0; i < desc->Desc_1_1.NumParameters; ++i) {
        const auto &parameter = desc->Desc_1_1.pParameters[i];
        if (parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            continue;
        UINT next = 0;
        for (UINT r = 0; r < parameter.DescriptorTable.NumDescriptorRanges; ++r) {
            const auto &range = parameter.DescriptorTable.pDescriptorRanges[r];
            const UINT offset =
                range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                    ? next
                    : range.OffsetInDescriptorsFromTableStart;
            // an unbounded range: as many as a table here reads at most
            const UINT count = range.NumDescriptors == UINT_MAX ? 64 : range.NumDescriptors;
            tables[i].push_back({range.RangeType, offset, count});
            next = offset + count;
        }
    }
    reader->Release();
    const std::lock_guard lock(signature_mutex);
    tables_of[signature] = std::move(tables);
}

// A descriptor the title wrote, made on the host as what the table it is bound in says it is.
// A buffer descriptor (V#) has a 48-bit address, the stride in bits 48-61 and the record
// count in its third dword.
std::atomic<std::uint64_t> decoded[4];
void decode_as(const ShadowHeap &heap, std::uintptr_t at, D3D12_DESCRIPTOR_RANGE_TYPE type) {
    std::uint32_t words[4];
    std::memcpy(words, reinterpret_cast<const void *>(at), sizeof words);
    std::uint64_t first;
    std::memcpy(&first, words, sizeof first);
    if (first == host_mark) {
        resolve(heap, at);
        return;
    }
    ++decoded[type & 3];
    if (static std::atomic<std::uint32_t> shown{0};
        std::getenv("XS_D3D_TRACE") && first != 0 && !(shown.fetch_or(1u << type) & (1u << type))) {
        std::uint32_t more[4]{};
        if (heap.stride >= 32)
            std::memcpy(more, reinterpret_cast<const void *>(at + 16), sizeof more);
        std::fprintf(stderr, "[Runtime] title %s descriptor: %08x %08x %08x %08x | %08x %08x %08x %08x"
                     "\n",
                     type == 0 ? "SRV" : type == 1 ? "UAV" : type == 2 ? "CBV" : "sampler",
                     words[0], words[1], words[2], words[3], more[0], more[1], more[2], more[3]);
        std::fflush(stderr);
    }
    // a descriptor the title cleared is a null one, whose reads give 0: the host's slot may
    // hold anything until it is made so
    if ((first == 0 && words[2] == 0 && words[3] == 0) || null_buffer(words)) {
        const auto host = host_slot(heap, at);
        auto *const device = heap.device;
        if (type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
            const D3D12_CONSTANT_BUFFER_VIEW_DESC view{};
            original<void(STDMETHODCALLTYPE *)(void *, const void *, D3D12_CPU_DESCRIPTOR_HANDLE)>(
                device, 17)(device, &view, host);
        } else if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            original<void(STDMETHODCALLTYPE *)(void *, void *, const void *,
                                               D3D12_CPU_DESCRIPTOR_HANDLE)>(device, 18)(
                device, nullptr, &view, host);
        } else if (type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_UINT;
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            original<void(STDMETHODCALLTYPE *)(void *, void *, void *, const void *,
                                               D3D12_CPU_DESCRIPTOR_HANDLE)>(device, 19)(
                device, nullptr, nullptr, &view, host);
        } else {
            return;
        }
        set_mark(heap, at);
        return;
    }
    const std::uint64_t address = first & 0xffff'ffff'ffffull;
    const UINT stride = (words[1] >> 16) & 0x3fff;
    const std::uint64_t bytes = std::uint64_t{words[2]} * (stride ? stride : 1);
    const auto host = host_slot(heap, at);
    auto *const device = heap.device;
    if (type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
        const D3D12_CONSTANT_BUFFER_VIEW_DESC view{
            address, static_cast<UINT>(std::min<std::uint64_t>((bytes + 255) & ~255ull, 65536))};
        original<void(STDMETHODCALLTYPE *)(void *, const void *, D3D12_CPU_DESCRIPTOR_HANDLE)>(
            device, 17)(device, &view, host);
    } else if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV || type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
        ID3D12Resource *resource = nullptr;
        std::uint64_t offset = 0, size = 0;
        if (!buffer_at(address, resource, offset, size))
            return;
        size = std::min(size, bytes ? bytes : size);
        // structured when the descriptor has a stride that is not a format's, raw otherwise
        const bool structured = stride > 4 && offset % stride == 0;
        const UINT element = structured ? stride : 4;
        if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer = {offset / element, static_cast<UINT>(size / element),
                           structured ? stride : 0,
                           structured ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW};
            original<void(STDMETHODCALLTYPE *)(void *, void *, const void *,
                                               D3D12_CPU_DESCRIPTOR_HANDLE)>(device, 18)(
                device, resource, &view, host);
        } else {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            view.Buffer = {offset / element, static_cast<UINT>(size / element),
                           structured ? stride : 0, 0,
                           structured ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW};
            original<void(STDMETHODCALLTYPE *)(void *, void *, void *, const void *,
                                               D3D12_CPU_DESCRIPTOR_HANDLE)>(device, 19)(
                device, resource, nullptr, &view, host);
        }
    } else {
        return;
    }
    set_mark(heap, at);
}
// A table bound at a GPU handle: what the title wrote in it, by the root signature's ranges
void decode_table(const void *signature, UINT parameter, std::uint64_t gpu) {
    std::vector<TableRange> ranges;
    {
        const std::lock_guard lock(signature_mutex);
        const auto found = tables_of.find(signature);
        if (found == tables_of.end() || parameter >= found->second.size())
            return;
        ranges = found->second[parameter];
    }
    ShadowHeap heap{};
    {
        const std::lock_guard lock(shadow_mutex);
        const auto *found = shadow_heap(gpu);
        if (!found || !found->shader_visible)
            return;
        heap = *found;
    }
    const std::uintptr_t table = gpu;
    for (const auto &range : ranges)
        for (UINT i = 0; i < range.count; ++i) {
            const auto at = table + std::uintptr_t{range.offset + i} * heap.stride;
            if (at + heap.stride > heap.end)
                break;
            decode_as(heap, at, range.type);
        }
}

// The device's methods that take CPU handles: the host's handle, and the shadow marked
using Handle = D3D12_CPU_DESCRIPTOR_HANDLE;
void STDMETHODCALLTYPE create_cbv(ID3D12Device *self, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
                                  Handle handle) {
    original<void(STDMETHODCALLTYPE *)(void *, const void *, Handle)>(self, 17)(
        self, desc, host_handle(handle));
    mark_host(handle);
}
void STDMETHODCALLTYPE create_srv(ID3D12Device *self, ID3D12Resource *resource, const void *desc,
                                  Handle handle) {
    original<void(STDMETHODCALLTYPE *)(void *, void *, const void *, Handle)>(self, 18)(
        self, resource, desc, host_handle(handle));
    mark_host(handle);
}
void STDMETHODCALLTYPE create_uav(ID3D12Device *self, ID3D12Resource *resource,
                                  ID3D12Resource *counter, const void *desc, Handle handle) {
    original<void(STDMETHODCALLTYPE *)(void *, void *, void *, const void *, Handle)>(self, 19)(
        self, resource, counter, desc, host_handle(handle));
    mark_host(handle);
}
// the resource behind each render target view, for XS_D3D_PROBE
std::mutex target_mutex;
std::map<std::uint64_t, ID3D12Resource *> target_of;
std::atomic<ID3D12Resource *> last_target{nullptr};
std::vector<ID3D12Resource *> bound_targets; // the first of each size, under target_mutex
template <std::size_t Index>
void STDMETHODCALLTYPE create_target_view(ID3D12Device *self, ID3D12Resource *resource,
                                          const void *desc, Handle handle) {
    original<void(STDMETHODCALLTYPE *)(void *, void *, const void *, Handle)>(self, Index)(
        self, resource, desc, host_handle(handle));
    mark_host(handle);
    if (Index == 20) {
        const std::lock_guard lock(target_mutex);
        target_of[host_handle(handle).ptr] = resource;
    }
}
void STDMETHODCALLTYPE create_sampler(ID3D12Device *self, const D3D12_SAMPLER_DESC *desc,
                                      Handle handle) {
    original<void(STDMETHODCALLTYPE *)(void *, const void *, Handle)>(self, 22)(
        self, desc, host_handle(handle));
    mark_host(handle);
}
// A copied descriptor: the host's goes along with the host's copy; one the title wrote goes
// along as the title wrote it, read where it is bound
void carry(Handle destination, Handle source, UINT stride) {
    host_view(source);
    const std::lock_guard lock(shadow_mutex);
    const auto *const heap = shadow_heap(destination.ptr);
    if (!heap)
        return;
    std::uint64_t first = host_mark;
    if (shadow_heap(source.ptr))
        std::memcpy(&first, reinterpret_cast<const void *>(source.ptr), sizeof first);
    if (first == host_mark || first == 0)
        set_mark(*heap, destination.ptr);
    else
        std::memcpy(reinterpret_cast<void *>(destination.ptr),
                    reinterpret_cast<const void *>(source.ptr), stride);
}
void STDMETHODCALLTYPE copy_descriptors(ID3D12Device *self, UINT destination_ranges,
                                        const Handle *destinations, const UINT *destination_sizes,
                                        UINT source_ranges, const Handle *sources,
                                        const UINT *source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    std::vector<Handle> host_destinations(destinations, destinations + destination_ranges);
    std::vector<Handle> host_sources(sources, sources + source_ranges);
    const UINT stride = console_stride(type);
    // the descriptors one by one, in the order both lists of ranges give them
    UINT source_range = 0, source_at = 0;
    for (UINT range = 0; range < destination_ranges; ++range) {
        const UINT count = destination_sizes ? destination_sizes[range] : 1;
        for (UINT i = 0; i < count && source_range < source_ranges; ++i) {
            carry({destinations[range].ptr + std::size_t{i} * stride},
                  {sources[source_range].ptr + std::size_t{source_at} * stride}, stride);
            if (++source_at >= (source_sizes ? source_sizes[source_range] : 1))
                ++source_range, source_at = 0;
        }
        host_destinations[range] = host_handle(destinations[range]);
    }
    for (UINT range = 0; range < source_ranges; ++range)
        host_sources[range] = host_handle(sources[range]);
    using Copy =
        void(STDMETHODCALLTYPE *)(void *, UINT, const Handle *, const UINT *, UINT, const Handle *,
                                  const UINT *, D3D12_DESCRIPTOR_HEAP_TYPE);
    original<Copy>(self, 23)(self, destination_ranges, host_destinations.data(), destination_sizes,
                             source_ranges, host_sources.data(), source_sizes, type);
}
void STDMETHODCALLTYPE copy_descriptors_simple(ID3D12Device *self, UINT count, Handle destination,
                                               Handle source, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    const UINT stride = console_stride(type);
    for (UINT i = 0; i < count; ++i)
        carry({destination.ptr + std::size_t{i} * stride}, {source.ptr + std::size_t{i} * stride},
              stride);
    using Copy =
        void(STDMETHODCALLTYPE *)(void *, UINT, Handle, Handle, D3D12_DESCRIPTOR_HEAP_TYPE);
    original<Copy>(self, 24)(self, count, host_handle(destination), host_handle(source), type);
}
// CreateDescriptorHeap: the heap's CPU handles are the console's (shadow memory), its GPU
// handles the host's, returned in RAX (9, 10)
HRESULT STDMETHODCALLTYPE create_descriptor_heap(ID3D12Device *self,
                                                 const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID iid,
                                                 void **heap) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **);
    const auto result = original<Create>(self, 14)(self, desc, iid, heap);
    if (FAILED(result) || !heap || !*heap || !desc)
        return result;
    auto *const host_heap_object = static_cast<ID3D12DescriptorHeap *>(*heap);
    const auto host = host_heap_object->GetCPUDescriptorHandleForHeapStart();
    const auto gpu = desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                         ? host_heap_object->GetGPUDescriptorHandleForHeapStart().ptr
                         : 0;
    auto *const proxy = new HeapProxy{heap_vtable.data(), host_heap_object, 1};
    {
        const std::lock_guard lock(heap_proxy_mutex);
        heap_proxies.insert(proxy);
    }
    *heap = proxy;
    register_shadow(self, proxy, *desc, host, gpu);
    return result;
}
HRESULT STDMETHODCALLTYPE create_committed_resource(ID3D12Device *, const D3D12_HEAP_PROPERTIES *,
                                                    D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                                                    D3D12_RESOURCE_STATES,
                                                    const D3D12_CLEAR_VALUE *, REFIID, void **);
void report_messages();
// CreateRootSignature (16): its descriptor tables are kept, to know what the descriptors the
// title writes are
HRESULT STDMETHODCALLTYPE create_root_signature(ID3D12Device *self, UINT node, const void *blob,
                                                SIZE_T size, REFIID iid, void **signature) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, UINT, const void *, SIZE_T, REFIID, void **);
    // A shader container says how long it is (the dword at 24). The host turns down a size
    // that differs from it; titles get away with a wrong one on the console (a sample passes
    // one shader with the size of another), so the container's own is what the host is given.
    std::uint32_t contained = 0;
    if (blob && size >= 32 && !std::memcmp(blob, "DXBC", 4)) {
        std::memcpy(&contained, static_cast<const std::uint8_t *>(blob) + 24, 4);
        MEMORY_BASIC_INFORMATION info{};
        const auto *const end = static_cast<const std::uint8_t *>(blob) + contained;
        if (contained >= 32 && contained != size && VirtualQuery(blob, &info, sizeof info) &&
            info.State == MEM_COMMIT &&
            end <= static_cast<const std::uint8_t *>(info.BaseAddress) + info.RegionSize)
            size = contained;
    }
    const auto result = original<Create>(self, 16)(self, node, blob, size, iid, signature);
    if (SUCCEEDED(result) && signature && *signature)
        read_root_signature(*signature, blob, size);
    if (FAILED(result)) {
        std::fprintf(stderr, "[Runtime] CreateRootSignature from %zu bytes: 0x%08lx\n",
                     static_cast<std::size_t>(size), static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// slot 44: CreatePlacedResourceX(address, desc, state, clear, iid, resource), a resource on
// memory of the title's (Unreal's XboxOneD3D12Texture.cpp names it). The host cannot place a
// resource at an address: a committed one in GPU memory stands for it.
HRESULT STDMETHODCALLTYPE create_placed_resource_x(ID3D12Device *self, std::uint64_t address,
                                                   const D3D12_RESOURCE_DESC *desc,
                                                   D3D12_RESOURCE_STATES state,
                                                   const D3D12_CLEAR_VALUE *clear, REFIID iid,
                                                   void **resource) {
    if (static std::atomic<int> shown{0};
        desc && std::getenv("XS_D3D_TRACE") &&
        (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || shown < 4) && shown++ < 40) {
        std::uint64_t first = 0;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
            info.State == MEM_COMMIT)
            std::memcpy(&first, reinterpret_cast<const void *>(address), sizeof first);
        std::fprintf(stderr,
                     "[Runtime] CreatePlacedResourceX at %llx: dimension %d %llux%ux%u mips %u "
                     "format %d layout %d flags 0x%x state 0x%x, memory %s %llx\n",
                     static_cast<unsigned long long>(address), desc->Dimension, desc->Width,
                     desc->Height, desc->DepthOrArraySize, desc->MipLevels, desc->Format,
                     desc->Layout, desc->Flags, state,
                     info.State == MEM_COMMIT ? "committed" : "not committed",
                     static_cast<unsigned long long>(first));
        std::fflush(stderr);
    }
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto result = create_committed_resource(self, &heap, D3D12_HEAP_FLAG_NONE, desc, state,
                                                  clear, iid, resource);
    // a texture the title fills in its memory: the host's copy follows it
    if (SUCCEEDED(result) && resource && *resource && desc &&
        !(desc->Flags &
          (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
        track_placement(static_cast<ID3D12Resource *>(*resource),
                                reinterpret_cast<const void *>(address));
    return result;
}
// The console's GPU draws rectangle lists and quad lists (topology types 19 and 32, the
// topologies 17 and 19). Here both are triangles: see draw_instanced.
D3D12_PRIMITIVE_TOPOLOGY_TYPE host_topology_type(D3D12_PRIMITIVE_TOPOLOGY_TYPE type) {
    return type == 19 || type == 32 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE : type;
}
// CreateGraphicsPipelineState (10): the console's flags, as for compute pipelines
HRESULT STDMETHODCALLTYPE create_graphics_pipeline(ID3D12Device *self,
                                                   const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                                   REFIID iid, void **state) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
                                                REFIID, void **);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC host{};
    if (desc) {
        host = *desc;
        host.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        host.PrimitiveTopologyType = host_topology_type(host.PrimitiveTopologyType);
    }
    const auto result = original<Create>(self, 10)(self, desc ? &host : nullptr, iid, state);
    if (FAILED(result)) {
        std::fprintf(stderr, "[Runtime] CreateGraphicsPipelineState: 0x%08lx\n",
                     static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// slot 74: RegisterPagePoolX(address, page count, handle): the pages are the title's memory,
// mapped for the GPU by later page mapping calls; here the pool is only a handle
HRESULT STDMETHODCALLTYPE register_page_pool(ID3D12Device *, std::uint64_t address, UINT,
                                             HANDLE *handle) {
    if (!handle)
        return E_POINTER;
    *handle = reinterpret_cast<HANDLE>(address);
    return S_OK;
}
// CreateComputePipelineState (11)
HRESULT STDMETHODCALLTYPE create_compute_pipeline(ID3D12Device *self,
                                                  const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                                                  REFIID iid, void **state) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *,
                                                REFIID, void **);
    // the console's pipeline flags mean nothing to the host, which takes none of them
    D3D12_COMPUTE_PIPELINE_STATE_DESC host{};
    if (desc) {
        host = *desc;
        host.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    }
    const auto result = original<Create>(self, 11)(self, desc ? &host : nullptr, iid, state);
    if (FAILED(result) && desc) {
        const auto *bytes = static_cast<const std::uint8_t *>(desc->CS.pShaderBytecode);
        std::fprintf(stderr,
                     "[Runtime] CreateComputePipelineState: 0x%08lx, shader of %zu bytes "
                     "starting %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     static_cast<unsigned long>(result), desc->CS.BytecodeLength,
                     bytes ? bytes[0] : 0, bytes ? bytes[1] : 0, bytes ? bytes[2] : 0,
                     bytes ? bytes[3] : 0, bytes ? bytes[4] : 0, bytes ? bytes[5] : 0,
                     bytes ? bytes[6] : 0, bytes ? bytes[7] : 0);
        // DXBC: magic, digest, version, size, chunk count, chunk offsets; each chunk a tag
        if (bytes && desc->CS.BytecodeLength >= 32) {
            std::uint32_t chunks = 0;
            std::memcpy(&chunks, bytes + 28, 4);
            std::string tags;
            for (std::uint32_t i = 0; i < chunks && 32 + i * 4 + 4 <= desc->CS.BytecodeLength;
                 ++i) {
                std::uint32_t offset = 0;
                std::memcpy(&offset, bytes + 32 + i * 4, 4);
                if (offset + 8 <= desc->CS.BytecodeLength) {
                    std::uint32_t size = 0;
                    std::memcpy(&size, bytes + offset + 4, 4);
                    tags += ' ';
                    tags.append(reinterpret_cast<const char *>(bytes + offset), 4);
                    tags += ':' + std::to_string(size);
                }
            }
            std::fprintf(stderr, "[Runtime] its chunks:%s\n", tags.c_str());
        }
        // XS_D3D_TRACE: the first shaders the host turned down, to look at
        if (static std::atomic<int> dumped{0};
            bytes && std::getenv("XS_D3D_TRACE") && std::getenv("TEMP") && dumped < 4)
            if (auto *const file = std::fopen((std::string(std::getenv("TEMP")) + "\\xs_shader_" +
                                               std::to_string(dumped++) + ".bin")
                                                  .c_str(),
                                              "wb")) {
                std::fwrite(bytes, 1, desc->CS.BytecodeLength, file);
                std::fclose(file);
            }
        std::fflush(stderr);
        report_messages();
    }
    return result;
}
// ID3D12CommandQueue goes on past the desktop's 19 methods with the console's own
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE unknown_queue_method(void *, std::uint64_t a, std::uint64_t b,
                                                     std::uint64_t c) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] unimplemented Direct3D 12 call: command queue slot %zu (%llx, "
                     "%llx, %llx) from %s\n",
                     Index, a, b, c, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return E_NOTIMPL;
}
void STDMETHODCALLTYPE execute_command_lists(ID3D12CommandQueue *self, UINT count,
                                             ID3D12CommandList *const *lists);
HRESULT STDMETHODCALLTYPE ignored_queue_method(void *) { return S_OK; }
// slot 23: SignalX(fence, value, flush flags), by Unreal's message; the host's caches need no
// flushing asked for
HRESULT STDMETHODCALLTYPE signal_x(ID3D12CommandQueue *self, ID3D12Fence *fence, UINT64 value,
                                   UINT) {
    return self->Signal(fence, value);
}
constexpr std::size_t host_queue_slots = 19, queue_slots = 48;
template <std::size_t... Index>
std::vector<std::pair<std::size_t, Slot>> unknown_queue_methods(std::index_sequence<Index...>) {
    return {{host_queue_slots + Index,
             reinterpret_cast<Slot>(&unknown_queue_method<host_queue_slots + Index>)}...};
}
// slot 62: CreateComputePipelineState as the console has it (Unreal's message names it): two
// more arguments, 0 at its call site, before the interface and the state
HRESULT STDMETHODCALLTYPE create_compute_pipeline_x(ID3D12Device *self,
                                                    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                                                    void *, void *, REFIID iid, void **state) {
    return create_compute_pipeline(self, desc, iid, state);
}
// The title's command queue: a proxy of the host's, which the host's runtime and DXGI keep as
// their own. The console's queue has the desktop's methods to Wait (15), then PIX captures,
// SuspendX, ResumeX, SignalX (23), ..., GetDesc (26) and its page mapping methods.
struct QueueProxy {
    Slot *vtable;
    ID3D12CommandQueue *host;
    std::atomic<ULONG> references;
};
ID3D12CommandQueue *queue_host(void *self) { return static_cast<QueueProxy *>(self)->host; }
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE queue_method(void *self, std::uint64_t a, std::uint64_t b,
                                             std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                             std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                             std::uint64_t i, std::uint64_t j) {
    auto *const host = queue_host(self);
    return reinterpret_cast<Raw>((*reinterpret_cast<Slot **>(host))[Index])(host, a, b, c, d, e, f,
                                                                            g, h, i, j);
}
HRESULT STDMETHODCALLTYPE queue_query(void *self, REFIID iid, void **object) {
    if (!object)
        return E_POINTER;
    if (is_any<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12CommandQueue>(
            iid)) {
        ++static_cast<QueueProxy *>(self)->references;
        *object = self;
        return S_OK;
    }
    return queue_host(self)->QueryInterface(iid, object);
}
ULONG STDMETHODCALLTYPE queue_add_ref(void *self) {
    return ++static_cast<QueueProxy *>(self)->references;
}
ULONG STDMETHODCALLTYPE queue_release(void *self) {
    auto *const proxy = static_cast<QueueProxy *>(self);
    const auto left = --proxy->references;
    if (left == 0) {
        proxy->host->Release();
        delete proxy;
    }
    return left;
}
void STDMETHODCALLTYPE queue_execute(void *self, UINT count, ID3D12CommandList *const *lists) {
    execute_command_lists(queue_host(self), count, lists);
}
HRESULT STDMETHODCALLTYPE queue_signal_x(void *self, ID3D12Fence *fence, UINT64 value, UINT) {
    return queue_host(self)->Signal(fence, value);
}
D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE queue_get_desc(void *self,
                                                           D3D12_COMMAND_QUEUE_DESC *desc) {
    using Get = D3D12_COMMAND_QUEUE_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_COMMAND_QUEUE_DESC *);
    auto *const host = queue_host(self);
    return reinterpret_cast<Get>((*reinterpret_cast<Slot **>(host))[18])(host, desc);
}
// slot 30: PresentX. The console shows the title's own resources; there is no swap chain.
HRESULT STDMETHODCALLTYPE queue_present_x(void *self, UINT planes, const void *plane_parameters,
                                          const void *present_parameters) {
    return present_x(queue_host(self), planes, plane_parameters, present_parameters);
}
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE unknown_queue_slot(void *, std::uint64_t a, std::uint64_t b) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] unimplemented Direct3D 12 call: command queue slot %zu (%llx, "
                     "%llx) from %s\n",
                     Index, a, b, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return E_NOTIMPL;
}
template <std::size_t... Index>
std::array<Slot, 48> queue_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, 48> vtable{(Index < 18
                                     ? reinterpret_cast<Slot>(&queue_method<Index>)
                                     : reinterpret_cast<Slot>(&unknown_queue_slot<Index>))...};
    vtable[0] = reinterpret_cast<Slot>(&queue_query);
    vtable[1] = reinterpret_cast<Slot>(&queue_add_ref);
    vtable[2] = reinterpret_cast<Slot>(&queue_release);
    vtable[10] = reinterpret_cast<Slot>(&queue_execute);
    for (const std::size_t ignored : {18, 19, 20, 21, 22, 24, 25})
        vtable[ignored] = reinterpret_cast<Slot>(&ignored_queue_method);
    vtable[23] = reinterpret_cast<Slot>(&queue_signal_x);
    vtable[26] = reinterpret_cast<Slot>(&queue_get_desc);
    vtable[30] = reinterpret_cast<Slot>(&queue_present_x);
    // SetHDRToneMapperX: the picture goes to the host's display as the title made it
    vtable[31] = reinterpret_cast<Slot>(&ignored_queue_method);
    return vtable;
}
std::array<Slot, 48> queue_vtable = queue_vtable_of(std::make_index_sequence<48>{});
void *make_queue_proxy(ID3D12CommandQueue *host) {
    return new QueueProxy{queue_vtable.data(), host, 1};
}
// slot 48: CreateCommandQueue(desc, iid, queue), by its arguments at Unreal's call site
HRESULT STDMETHODCALLTYPE create_command_queue(ID3D12Device *self,
                                               const D3D12_COMMAND_QUEUE_DESC *desc, REFIID iid,
                                               void **queue) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);
    const auto result = original<Create>(self, 8)(self, desc, iid, queue);
    auto *const host =
        SUCCEEDED(result) && queue ? static_cast<ID3D12CommandQueue *>(*queue) : nullptr;
    if (host)
        *queue = make_queue_proxy(host);
    if (SUCCEEDED(result) && queue && *queue && desc &&
        desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12CommandQueue *expected = nullptr;
        if (first_direct_queue.compare_exchange_strong(expected, host))
            first_direct_queue.load()->AddRef();
    }
    return result;
}
// CreateCommittedResource, reported when the host turns it down
HRESULT STDMETHODCALLTYPE create_committed_resource(
    ID3D12Device *self, const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
    REFIID iid, void **resource) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(
        void *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
    // the console's D3D12_RESOURCE_STATE_GENERIC_READ (0x8c3) has no indirect argument bit, the
    // desktop's (0xac3), which upload heaps require, does
    constexpr auto console_generic_read = static_cast<D3D12_RESOURCE_STATES>(0x8c3);
    if (state == console_generic_read)
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    // resource flags above the desktop's (0x100000, 0x200000 on Unreal's render targets) are
    // the console's own
    D3D12_RESOURCE_DESC host_desc{};
    if (desc) {
        host_desc = *desc;
        host_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host_desc.Flags & 0xff);
        // a console layout names a tile mode (0x100 | mode): the host picks its own
        if (host_desc.Layout >= 0x100)
            host_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    const auto result = original<Create>(self, 27)(self, heap, flags, desc ? &host_desc : nullptr,
                                                   state, clear, iid, resource);
    if (SUCCEEDED(result) && resource && *resource) {
        console_resource(*resource);
        track_buffer(static_cast<ID3D12Resource *>(*resource));
    }
    if (FAILED(result) && heap && desc) {
        std::fprintf(stderr,
                     "[Runtime] CreateCommittedResource heap %d/%d/%d flags 0x%x dimension %d "
                     "alignment %llu %llux%ux%u mips %u format %d samples %u layout %d flags "
                     "0x%x state 0x%x: 0x%08lx\n",
                     heap->Type, heap->CPUPageProperty, heap->MemoryPoolPreference, flags,
                     desc->Dimension, desc->Alignment, desc->Width, desc->Height,
                     desc->DepthOrArraySize, desc->MipLevels, desc->Format, desc->SampleDesc.Count,
                     desc->Layout, desc->Flags, state, static_cast<unsigned long>(result));
        std::fflush(stderr);
    }
    return result;
}
// XS_D3D_DEBUG: the host's debug layer, whose messages go to the log when something the title
// asked for is turned down
std::atomic<ID3D12InfoQueue *> debug_messages{nullptr};
void report_messages() {
    auto *const queue = debug_messages.load();
    if (!queue)
        return;
    for (UINT64 i = 0, count = queue->GetNumStoredMessages(); i < count; ++i) {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<std::uint8_t> buffer(size);
        auto *const message = reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
        if (SUCCEEDED(queue->GetMessage(i, message, &size)) &&
            message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            std::fprintf(stderr, "[Runtime] Direct3D 12 debug layer: %.*s\n",
                         static_cast<int>(message->DescriptionByteLength), message->pDescription);
    }
    queue->ClearStoredMessages();
    std::fflush(stderr);
}

// ID3D12GraphicsCommandList as the console has it. The title reads the object itself: a
// write pointer (at +0x38 on Xbox One, +0x18 on Xbox Series) and its limits after it, between
// which it records commands inline when there is room. The title gets a proxy of the
// console's size whose inline space is always full, so that every command comes through a
// method, and the proxy hands those to the host's list. On Xbox Series the inline
// SetPipelineState also copies 32 bytes of the pipeline state into the list at +0x540, before
// it looks at the write pointer: the proxy has the room, 0x560 bytes being the list's size
// there.
struct ListProxy {
    Slot *vtable;
    std::uint8_t fields[0x600 - sizeof(Slot *)];
    ID3D12GraphicsCommandList *host;
    std::atomic<ULONG> references;
    ID3D12Resource *target = nullptr; // bound last, for XS_D3D_PROBE's draw counts
    const void *graphics_root = nullptr, *compute_root = nullptr;
    ID3D12DescriptorHeap *heaps[2]{}; // the host's, as bound: views, then samplers
    UINT topology = 0;                 // as the title set it, the console's own included
    D3D12_INDEX_BUFFER_VIEW indices{}; // the title's, to put back after a quad draw
};
std::mutex proxy_mutex;
std::set<const void *> list_proxies;
// the host's list for a title's one, which may be either
ID3D12CommandList *host_list(void *list) {
    const std::lock_guard lock(proxy_mutex);
    return list_proxies.contains(list) ? static_cast<ListProxy *>(list)->host
                                       : static_cast<ID3D12CommandList *>(list);
}
using List = ID3D12GraphicsCommandList;
List *host_of(void *self) { return static_cast<ListProxy *>(self)->host; }

// Each console slot calls this host slot, the arguments going on as they came (integer
// registers and stack). The console has the desktop's methods in another order: the queries
// (BeginQuery, EndQuery, ResolveQueryData, SetPredication; the desktop's 52 to 55) come right
// after ExecuteBundle, at 28 to 31, and push SetDescriptorHeaps to ClearUnorderedAccessView-
// Float 4 slots later; from SetMarker (56) on they agree again. Unreal's call sites show it:
// Close, Reset, CopyBufferRegion and ResourceBarrier in place, SetDescriptorHeaps at 32, its
// timestamps' EndQuery and ResolveQueryData at 29 and 30.
//
// Past the first interface the two consoles differ. Xbox One has ID3D12GraphicsCommandList1
// and 2 at 104 to 110. Xbox Series has two more methods of its own first (to 104), then
// those at 105 to 111 and ID3D12GraphicsCommandList3 to 6 at 112 to 128, with four console
// methods among them (122, 123, 126, 128). The host has all of them from 60 on, in the
// desktop's order.
constexpr std::size_t host_list_slots = 60, list_slots = 136, unmapped = ~std::size_t{0};
std::array<std::size_t, list_slots> list_map{};
void map_list_slots() {
    for (std::size_t slot = 0; slot < list_slots; ++slot)
        list_map[slot] = slot <= 27               ? slot
                         : slot < 32              ? slot + 24
                         : slot < 56              ? slot - 4
                         : slot < host_list_slots ? slot
                                                  : unmapped;
    // CopyTextureRegion with one more argument, which the host's has not
    list_map[83] = 16;
    // DispatchX and CopyResourceX likewise: Dispatch and CopyResource, and flags
    list_map[78] = 14;
    list_map[84] = 17;
    if (console == Target::durango) {
        for (std::size_t slot = 104; slot <= 110; ++slot)
            list_map[slot] = slot - 44;
        return;
    }
    for (std::size_t slot = 105; slot <= 121; ++slot)
        list_map[slot] = slot - 45;
    list_map[124] = 77; // RSSetShadingRate
    list_map[125] = 78; // RSSetShadingRateImage
    list_map[127] = 79; // DispatchMesh
}
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE list_method(void *self, std::uint64_t a, std::uint64_t b,
                                            std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                            std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                            std::uint64_t i, std::uint64_t j) {
    static const bool tracing = std::getenv("XS_D3D_TRACE") != nullptr;
    if (static std::atomic<int> calls{0}; tracing && calls++ < 2) {
        std::fprintf(
            stderr,
            "[Runtime] Direct3D 12 command list slot %zu (%llx %llx %llx %llx %llx) from %s\n",
            Index, a, b, c, d, e, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    const auto target = list_map[Index];
    if (target == unmapped) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
            std::fprintf(stderr,
                         "[Runtime] unimplemented Direct3D 12 call: command list slot %zu (%llx, "
                         "%llx) from %s\n",
                         Index, a, b, detail::guest_location(_ReturnAddress()).c_str());
            std::fflush(stderr);
        }
        return E_NOTIMPL;
    }
    auto *const host = host_of(self);
    return reinterpret_cast<Raw>((*reinterpret_cast<Slot **>(host))[target])(host, a, b, c, d, e, f,
                                                                             g, h, i, j);
}
HRESULT STDMETHODCALLTYPE list_query(void *self, REFIID iid, void **object) {
    if (!object)
        return E_POINTER;
    // The later interfaces are the same object on the console, and on the host: the proxy
    // answers for those the host's list has. ID3D12GraphicsCommandList5 and 6 had other
    // interface IDs in the kits than the ones the desktop shipped with.
    static constexpr GUID kit_list5{
        0x55050859, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
    static constexpr GUID kit_list6{
        0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
    const auto &host_iid = iid == kit_list5   ? __uuidof(ID3D12GraphicsCommandList5)
                           : iid == kit_list6 ? __uuidof(ID3D12GraphicsCommandList6)
                                              : iid;
    bool ours = is_any<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12CommandList,
                       ID3D12GraphicsCommandList>(host_iid);
    if (!ours && is_any<ID3D12GraphicsCommandList1, ID3D12GraphicsCommandList2,
                        ID3D12GraphicsCommandList3, ID3D12GraphicsCommandList4,
                        ID3D12GraphicsCommandList5, ID3D12GraphicsCommandList6>(host_iid)) {
        IUnknown *later = nullptr;
        if (SUCCEEDED(host_of(self)->QueryInterface(host_iid, reinterpret_cast<void **>(&later)))) {
            later->Release();
            ours = true;
        }
    }
    if (ours) {
        ++static_cast<ListProxy *>(self)->references;
        *object = self;
        return S_OK;
    }
    return host_of(self)->QueryInterface(host_iid, object);
}
ULONG STDMETHODCALLTYPE list_add_ref(void *self) {
    return ++static_cast<ListProxy *>(self)->references;
}
ULONG STDMETHODCALLTYPE list_release(void *self) {
    auto *const proxy = static_cast<ListProxy *>(self);
    const auto left = --proxy->references;
    if (left == 0) {
        {
            const std::lock_guard lock(proxy_mutex);
            list_proxies.erase(proxy);
        }
        proxy->host->Release();
        delete proxy;
    }
    return left;
}

// The methods that take CPU handles, at their console slots (the desktop's + 4): the host's
// handles. The depth clear has a float in a register the generic path does not carry.
using Handle = D3D12_CPU_DESCRIPTOR_HANDLE;
std::atomic<ID3D12Device *> d3d12_device{nullptr};
HRESULT STDMETHODCALLTYPE close_list(void *self) {
    const auto result = host_of(self)->Close();
    if (FAILED(result))
        report_messages();
    return result;
}
// ResourceBarrier: transitions name states the console's way (GENERIC_READ is 0x8c3 there)
D3D12_RESOURCE_STATES host_state(D3D12_RESOURCE_STATES state) {
    return state == static_cast<D3D12_RESOURCE_STATES>(0x8c3) ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                              : state;
}
// The state the title left each render target in, by its last transition: the console shows
// a resource whatever its state, the host copies it out of one it has to be told
std::mutex state_mutex;
std::map<ID3D12Resource *, D3D12_RESOURCE_STATES> target_states;
void STDMETHODCALLTYPE resource_barrier(void *self, UINT count,
                                        const D3D12_RESOURCE_BARRIER *barriers) {
    std::vector<D3D12_RESOURCE_BARRIER> host(barriers, barriers + (barriers ? count : 0));
    for (auto &barrier : host)
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            barrier.Transition.StateBefore = host_state(barrier.Transition.StateBefore);
            barrier.Transition.StateAfter = host_state(barrier.Transition.StateAfter);
            if (barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_RENDER_TARGET ||
                barrier.Transition.StateBefore & D3D12_RESOURCE_STATE_RENDER_TARGET) {
                const std::lock_guard lock(state_mutex);
                target_states[barrier.Transition.pResource] = barrier.Transition.StateAfter;
            }
        }
    host_of(self)->ResourceBarrier(count, host.data());
}
void STDMETHODCALLTYPE execute_bundle(void *self, ID3D12GraphicsCommandList *bundle) {
    host_of(self)->ExecuteBundle(static_cast<List *>(host_list(bundle)));
}
// XS_D3D_TRACE: the first calls of a method, with a handle and the host's for it
void trace_handle(const char *method, Handle handle) {
    static const bool tracing = std::getenv("XS_D3D_TRACE") != nullptr;
    static std::atomic<int> calls{0};
    if (tracing && calls++ < 24) {
        std::fprintf(stderr, "[Runtime] Direct3D 12 %s(%llx -> %llx)\n", method,
                     static_cast<unsigned long long>(handle.ptr),
                     static_cast<unsigned long long>(host_handle(handle).ptr));
        std::fflush(stderr);
    }
}
// SetDescriptorHeaps: the host's heaps for the title's proxies
void STDMETHODCALLTYPE set_descriptor_heaps(void *self, UINT count,
                                            ID3D12DescriptorHeap *const *heaps) {
    auto *const list = static_cast<ListProxy *>(self);
    ID3D12DescriptorHeap *host[2]{};
    list->heaps[0] = list->heaps[1] = nullptr;
    for (UINT i = 0; i < count && i < 2; ++i) {
        host[i] = host_heap(heaps[i]);
        list->heaps[host[i]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] = host[i];
    }
    host_of(self)->SetDescriptorHeaps(std::min(count, 2u), host);
}
// A console GPU handle is the descriptor's address, good whatever heaps are set, and titles
// use tables of heaps they never set. The host reads a table from the heap that is bound,
// so the heap a handle is in gets bound first. It replaces the one of its kind: tables of
// two view heaps cannot be bound for one draw here.
void bind_heap_of(void *self, std::uint64_t handle) {
    void *owner = nullptr;
    bool samplers = false;
    {
        const std::lock_guard lock(shadow_mutex);
        const auto *heap = shadow_heap(handle);
        if (!heap || !heap->shader_visible)
            return;
        owner = heap->owner;
        samplers = heap->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    }
    auto *const list = static_cast<ListProxy *>(self);
    auto *const host = host_heap(owner);
    if (list->heaps[samplers] == host)
        return;
    list->heaps[samplers] = host;
    ID3D12DescriptorHeap *bound[2]{};
    UINT count = 0;
    for (auto *heap : list->heaps)
        if (heap)
            bound[count++] = heap;
    host_of(self)->SetDescriptorHeaps(count, bound);
}
// Reset: the host's list starts again with no heaps
HRESULT STDMETHODCALLTYPE reset_list(void *self, ID3D12CommandAllocator *allocator,
                                     ID3D12PipelineState *state) {
    auto *const list = static_cast<ListProxy *>(self);
    list->heaps[0] = list->heaps[1] = nullptr;
    list->topology = 0;
    list->indices = {};
    return host_of(self)->Reset(allocator, state);
}
// the host's GPU handle for a console one, which is the descriptor's address in the shadow
D3D12_GPU_DESCRIPTOR_HANDLE host_gpu(D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    const std::lock_guard lock(shadow_mutex);
    if (const auto *heap = shadow_heap(handle.ptr); heap && heap->shader_visible)
        return {heap->gpu + (handle.ptr - heap->base) / heap->stride * heap->host_stride};
    return handle;
}
// The root signatures and descriptor tables: the title's own descriptors in a table are read
// as the table says, once bound
void STDMETHODCALLTYPE set_compute_root_signature(void *self, ID3D12RootSignature *signature) {
    static_cast<ListProxy *>(self)->compute_root = signature;
    host_of(self)->SetComputeRootSignature(signature);
}
void STDMETHODCALLTYPE set_graphics_root_signature(void *self, ID3D12RootSignature *signature) {
    static_cast<ListProxy *>(self)->graphics_root = signature;
    host_of(self)->SetGraphicsRootSignature(signature);
}
void STDMETHODCALLTYPE set_compute_root_table(void *self, UINT parameter,
                                              D3D12_GPU_DESCRIPTOR_HANDLE table) {
    decode_table(static_cast<ListProxy *>(self)->compute_root, parameter, table.ptr);
    bind_heap_of(self, table.ptr);
    host_of(self)->SetComputeRootDescriptorTable(parameter, host_gpu(table));
}
void STDMETHODCALLTYPE set_graphics_root_table(void *self, UINT parameter,
                                               D3D12_GPU_DESCRIPTOR_HANDLE table) {
    decode_table(static_cast<ListProxy *>(self)->graphics_root, parameter, table.ptr);
    bind_heap_of(self, table.ptr);
    host_of(self)->SetGraphicsRootDescriptorTable(parameter, host_gpu(table));
}
// XS_D3D_PROBE: draws per render target
std::mutex draw_mutex;
std::map<ID3D12Resource *, std::uint64_t> draws_into;
void count_draw(void *self) {
    static const bool probing = std::getenv("XS_D3D_PROBE") != nullptr;
    if (!probing)
        return;
    const std::lock_guard lock(draw_mutex);
    ++draws_into[static_cast<ListProxy *>(self)->target];
}
constexpr UINT rectangle_list = 17, quad_list = 19;
void STDMETHODCALLTYPE set_topology(void *self, UINT topology) {
    static_cast<ListProxy *>(self)->topology = topology;
    host_of(self)->IASetPrimitiveTopology(topology == rectangle_list || topology == quad_list
                                              ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                              : static_cast<D3D_PRIMITIVE_TOPOLOGY>(topology));
}
// Indices that draw quads as two triangles each, for at least `quads` of them: 0 1 2, 0 2 3,
// then the same four vertices on. One buffer that only grows; the ones it outgrew stay
// alive, since lists already recorded may still use them.
D3D12_INDEX_BUFFER_VIEW quad_indices(UINT quads) {
    static std::mutex mutex;
    static ID3D12Resource *buffer = nullptr;
    static UINT capacity = 0;
    const std::lock_guard lock(mutex);
    auto *const device = d3d12_device.load();
    if (quads > capacity && device) {
        const UINT wanted = std::max(quads, std::max(capacity * 2, 4096u));
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                       0,
                                       std::uint64_t{wanted} * 6 * sizeof(std::uint32_t),
                                       1,
                                       1,
                                       1,
                                       DXGI_FORMAT_UNKNOWN,
                                       {1, 0},
                                       D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                       D3D12_RESOURCE_FLAG_NONE};
        ID3D12Resource *larger = nullptr;
        void *mapped = nullptr;
        if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&larger))) &&
            SUCCEEDED(larger->Map(0, nullptr, &mapped))) {
            auto *const index = static_cast<std::uint32_t *>(mapped);
            constexpr UINT corners[] = {0, 1, 2, 0, 2, 3};
            for (UINT quad = 0; quad < wanted; ++quad)
                for (UINT corner = 0; corner < 6; ++corner)
                    index[quad * 6 + corner] = quad * 4 + corners[corner];
            larger->Unmap(0, nullptr);
            buffer = larger;
            capacity = wanted;
        }
    }
    if (!buffer || quads > capacity)
        return {};
    return {buffer->GetGPUVirtualAddress(), quads * 6 * static_cast<UINT>(sizeof(std::uint32_t)),
            DXGI_FORMAT_R32_UINT};
}
// A quad list is drawn with indices of ours over the title's vertices. A rectangle list
// gives three corners of each rectangle and the GPU makes the fourth: here only the
// triangle of the three is drawn, half of each rectangle.
void STDMETHODCALLTYPE draw_instanced(void *self, UINT vertices, UINT instances, UINT first,
                                      UINT first_instance) {
    count_draw(self);
    auto *const list = static_cast<ListProxy *>(self);
    if (list->topology == quad_list) {
        const auto view = quad_indices(vertices / 4);
        if (!view.BufferLocation)
            return;
        list->host->IASetIndexBuffer(&view);
        list->host->DrawIndexedInstanced(vertices / 4 * 6, instances, 0, static_cast<INT>(first),
                                         first_instance);
        list->host->IASetIndexBuffer(list->indices.BufferLocation ? &list->indices : nullptr);
        return;
    }
    if (static std::atomic<bool> shown{false};
        list->topology == rectangle_list && !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] a rectangle list is drawn as the triangles of its three "
                             "corners: half of each rectangle is missing\n");
        std::fflush(stderr);
    }
    host_of(self)->DrawInstanced(vertices, instances, first, first_instance);
}
void STDMETHODCALLTYPE draw_indexed_instanced(void *self, UINT indices, UINT instances, UINT first,
                                              INT base, UINT first_instance) {
    count_draw(self);
    if (static std::atomic<bool> shown{false};
        static_cast<ListProxy *>(self)->topology == quad_list && !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] an indexed quad list is drawn as a triangle list, which "
                             "is not the same picture\n");
        std::fflush(stderr);
    }
    host_of(self)->DrawIndexedInstanced(indices, instances, first, base, first_instance);
}
// XS_D3D_TRACE: a GPU address the title gives, and whether it is CPU memory of the process
void trace_address(const char *method, std::uint64_t address) {
    static const bool tracing = std::getenv("XS_D3D_TRACE") != nullptr;
    static std::atomic<int> calls{0};
    if (!tracing || calls++ >= 24)
        return;
    MEMORY_BASIC_INFORMATION info{};
    const bool cpu = VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
                     info.State == MEM_COMMIT;
    std::fprintf(stderr, "[Runtime] Direct3D 12 %s(%llx)%s\n", method,
                 static_cast<unsigned long long>(address), cpu ? " in the title's memory" : "");
    std::fflush(stderr);
}
void STDMETHODCALLTYPE set_vertex_buffers(void *self, UINT start, UINT count,
                                          const D3D12_VERTEX_BUFFER_VIEW *views) {
    if (views && count)
        trace_address("IASetVertexBuffers", views->BufferLocation);
    host_of(self)->IASetVertexBuffers(start, count, views);
}
void STDMETHODCALLTYPE set_index_buffer(void *self, const D3D12_INDEX_BUFFER_VIEW *view) {
    if (view)
        trace_address("IASetIndexBuffer", view->BufferLocation);
    static_cast<ListProxy *>(self)->indices = view ? *view : D3D12_INDEX_BUFFER_VIEW{};
    host_of(self)->IASetIndexBuffer(view);
}
void STDMETHODCALLTYPE set_graphics_root_cbv(void *self, UINT index, std::uint64_t address) {
    trace_address("SetGraphicsRootConstantBufferView", address);
    host_of(self)->SetGraphicsRootConstantBufferView(index, address);
}
void STDMETHODCALLTYPE set_viewports(void *self, UINT count, const D3D12_VIEWPORT *viewports) {
    static const bool tracing = std::getenv("XS_D3D_TRACE") != nullptr;
    if (static std::atomic<int> calls{0}; tracing && viewports && count && calls++ < 6) {
        std::fprintf(stderr, "[Runtime] Direct3D 12 RSSetViewports(%u: %g %g %g %g %g %g)\n", count,
                     viewports->TopLeftX, viewports->TopLeftY, viewports->Width, viewports->Height,
                     viewports->MinDepth, viewports->MaxDepth);
        std::fflush(stderr);
    }
    host_of(self)->RSSetViewports(count, viewports);
}
void STDMETHODCALLTYPE set_scissors(void *self, UINT count, const D3D12_RECT *rects) {
    static const bool tracing = std::getenv("XS_D3D_TRACE") != nullptr;
    if (static std::atomic<int> calls{0}; tracing && rects && count && calls++ < 6) {
        std::fprintf(stderr, "[Runtime] Direct3D 12 RSSetScissorRects(%u: %ld %ld %ld %ld)\n",
                     count, rects->left, rects->top, rects->right, rects->bottom);
        std::fflush(stderr);
    }
    host_of(self)->RSSetScissorRects(count, rects);
}
void STDMETHODCALLTYPE om_set_render_targets(void *self, UINT count, const Handle *targets,
                                             BOOL single, const Handle *depth) {
    if (targets && count)
        trace_handle("OMSetRenderTargets", targets[0]);
    std::vector<Handle> host(targets ? (single ? 1 : count) : 0);
    for (std::size_t i = 0; i < host.size(); ++i)
        host[i] = host_view(targets[i]);
    // one handle for a run of descriptors: the rest follow it in the heap
    const auto stride = console_stride(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 1; targets && single && i < count; ++i)
        host_view({targets[0].ptr + std::size_t{i} * stride});
    if (!host.empty()) {
        const std::lock_guard lock(target_mutex);
        if (const auto found = target_of.find(host[0].ptr); found != target_of.end()) {
            last_target = found->second;
            static_cast<ListProxy *>(self)->target = found->second;
            const auto desc = found->second->GetDesc();
            if (bound_targets.size() < 12 &&
                std::none_of(bound_targets.begin(), bound_targets.end(), [&](auto *other) {
                    const auto known = other->GetDesc();
                    return known.Width == desc.Width && known.Height == desc.Height &&
                           known.Format == desc.Format;
                }))
                bound_targets.push_back(found->second);
        }
    }
    Handle host_depth{};
    if (depth)
        host_depth = host_view(*depth);
    host_of(self)->OMSetRenderTargets(count, targets ? host.data() : nullptr, single,
                                      depth ? &host_depth : nullptr);
}
void STDMETHODCALLTYPE clear_depth_stencil(void *self, Handle view, D3D12_CLEAR_FLAGS flags,
                                           FLOAT depth, UINT8 stencil, UINT count,
                                           const D3D12_RECT *rects) {
    host_of(self)->ClearDepthStencilView(host_view(view), flags, depth, stencil, count, rects);
}
void STDMETHODCALLTYPE clear_render_target(void *self, Handle view, const FLOAT *color, UINT count,
                                           const D3D12_RECT *rects) {
    host_of(self)->ClearRenderTargetView(host_view(view), color, count, rects);
}
void STDMETHODCALLTYPE clear_uav_uint(void *self, D3D12_GPU_DESCRIPTOR_HANDLE gpu, Handle cpu,
                                      ID3D12Resource *resource, const UINT *values, UINT count,
                                      const D3D12_RECT *rects) {
    bind_heap_of(self, gpu.ptr);
    host_of(self)->ClearUnorderedAccessViewUint(host_gpu(gpu), host_view(cpu), resource, values,
                                                count, rects);
}
void STDMETHODCALLTYPE clear_uav_float(void *self, D3D12_GPU_DESCRIPTOR_HANDLE gpu, Handle cpu,
                                       ID3D12Resource *resource, const FLOAT *values, UINT count,
                                       const D3D12_RECT *rects) {
    bind_heap_of(self, gpu.ptr);
    host_of(self)->ClearUnorderedAccessViewFloat(host_gpu(gpu), host_view(cpu), resource, values,
                                                 count, rects);
}
void STDMETHODCALLTYPE ignored_list_method(void *) {}
template <std::size_t... Index>
std::array<Slot, list_slots> list_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, list_slots> vtable{reinterpret_cast<Slot>(&list_method<Index>)...};
    vtable[0] = reinterpret_cast<Slot>(&list_query);
    vtable[1] = reinterpret_cast<Slot>(&list_add_ref);
    vtable[2] = reinterpret_cast<Slot>(&list_release);
    vtable[9] = reinterpret_cast<Slot>(&close_list);
    vtable[10] = reinterpret_cast<Slot>(&reset_list);
    vtable[21] = reinterpret_cast<Slot>(&set_viewports);
    vtable[22] = reinterpret_cast<Slot>(&set_scissors);
    vtable[26] = reinterpret_cast<Slot>(&resource_barrier);
    vtable[27] = reinterpret_cast<Slot>(&execute_bundle);
    vtable[20] = reinterpret_cast<Slot>(&set_topology);
    vtable[12] = reinterpret_cast<Slot>(&draw_instanced);
    vtable[13] = reinterpret_cast<Slot>(&draw_indexed_instanced);
    vtable[32] = reinterpret_cast<Slot>(&set_descriptor_heaps);
    vtable[33] = reinterpret_cast<Slot>(&set_compute_root_signature);
    vtable[34] = reinterpret_cast<Slot>(&set_graphics_root_signature);
    vtable[35] = reinterpret_cast<Slot>(&set_compute_root_table);
    vtable[36] = reinterpret_cast<Slot>(&set_graphics_root_table);
    vtable[42] = reinterpret_cast<Slot>(&set_graphics_root_cbv);
    vtable[47] = reinterpret_cast<Slot>(&set_index_buffer);
    vtable[48] = reinterpret_cast<Slot>(&set_vertex_buffers);
    vtable[50] = reinterpret_cast<Slot>(&om_set_render_targets);
    vtable[51] = reinterpret_cast<Slot>(&clear_depth_stencil);
    vtable[52] = reinterpret_cast<Slot>(&clear_render_target);
    vtable[53] = reinterpret_cast<Slot>(&clear_uav_uint);
    vtable[54] = reinterpret_cast<Slot>(&clear_uav_float);
    // FlushPipelineX and OMSetDepthBoundsX: cache flushes the host makes itself, and a depth
    // bounds test no host pipeline here enables
    vtable[67] = reinterpret_cast<Slot>(&ignored_list_method);
    vtable[68] = reinterpret_cast<Slot>(&ignored_list_method);
    // what tunes the console's GPU and changes no picture: occlusion query and hierarchical
    // stencil controls, NopX, KickoffX, shader limits and compression state
    for (const std::size_t hint : {64, 65, 66, 73, 79, 82, 93, 95, 96, 97})
        vtable[hint] = reinterpret_cast<Slot>(&ignored_list_method);
    return vtable;
}
std::array<Slot, list_slots> list_vtable = list_vtable_of(std::make_index_sequence<list_slots>{});
// BeginRenderPass: the views in its descriptions are console handles
void STDMETHODCALLTYPE begin_render_pass(void *self, UINT count,
                                         const D3D12_RENDER_PASS_RENDER_TARGET_DESC *targets,
                                         const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth,
                                         D3D12_RENDER_PASS_FLAGS flags) {
    ID3D12GraphicsCommandList4 *host = nullptr;
    if (FAILED(host_of(self)->QueryInterface(IID_PPV_ARGS(&host))))
        return;
    std::vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> host_targets(
        targets, targets + (targets ? count : 0));
    for (auto &target : host_targets)
        target.cpuDescriptor = host_view(target.cpuDescriptor);
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC host_depth{};
    if (depth) {
        host_depth = *depth;
        host_depth.cpuDescriptor = host_view(depth->cpuDescriptor);
    }
    host->BeginRenderPass(count, host_targets.data(), depth ? &host_depth : nullptr, flags);
    host->Release();
}
// DispatchMeshX (group count, flags, indirect arguments, payload): only the plain form, with
// a count and nothing else, has a host method
void STDMETHODCALLTYPE dispatch_mesh_x(void *self, UINT groups, UINT64 flags, UINT64 arguments,
                                       UINT64 payload) {
    ID3D12GraphicsCommandList6 *host = nullptr;
    if (flags || arguments || payload ||
        FAILED(host_of(self)->QueryInterface(IID_PPV_ARGS(&host)))) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true))
            std::fprintf(stderr,
                         "[Runtime] DispatchMeshX(%u, flags 0x%llx, arguments %llx, payload "
                         "%llx) has no host form\n",
                         groups, flags, arguments, payload);
        return;
    }
    host->DispatchMesh(groups, 1, 1);
    host->Release();
}
// Write64BitValueBottomOfPipeX: the console's GPU writes a value, or its clock, to memory
// of the title's when it gets there. Titles time their frames with it. Here the write
// happens when the list is recorded, with the host's clock for a timestamp (flag 1).
void STDMETHODCALLTYPE write_64_bottom_of_pipe(void *, std::uint64_t destination,
                                               std::uint64_t value, UINT, UINT flags) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void *>(destination), &info, sizeof info) ||
        info.State != MEM_COMMIT || !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        return;
    if (flags & 1) {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        value = static_cast<std::uint64_t>(counter.QuadPart);
    }
    std::memcpy(reinterpret_cast<void *>(destination), &value, sizeof value);
}
// the slots where the two consoles' lists differ
void configure_lists() {
    map_list_slots();
    list_vtable[70] = reinterpret_cast<Slot>(&write_64_bottom_of_pipe);
    const auto ignore = [](std::initializer_list<std::size_t> slots) {
        for (const auto slot : slots)
            list_vtable[slot] = reinterpret_cast<Slot>(&ignored_list_method);
    };
    if (console == Target::durango) {
        // PrefetchMemoryX and PrefetchPipelineStateX
        ignore({98, 99});
        return;
    }
    list_vtable[98] = reinterpret_cast<Slot>(&dispatch_mesh_x);
    // DriverHintX, the counters, and the binning of primitive batches
    ignore({99, 100, 101, 102, 103, 104});
    list_vtable[113] = reinterpret_cast<Slot>(&begin_render_pass);
}
void *make_list_proxy(List *host) {
    auto *const proxy = new ListProxy{};
    proxy->vtable = list_vtable.data();
    proxy->host = host;
    proxy->references = 1;
    const std::lock_guard lock(proxy_mutex);
    list_proxies.insert(proxy);
    return proxy;
}

// slot 73: a command list from a description (Unreal's call site: the type, 0, the node mask,
// 0), an allocator, a pipeline state, then the interface and the list
HRESULT STDMETHODCALLTYPE create_command_list(ID3D12Device *self, const UINT *desc,
                                              ID3D12CommandAllocator *allocator,
                                              ID3D12PipelineState *state, REFIID, void **list) {
    if (!desc || !list)
        return E_INVALIDARG;
    List *host = nullptr;
    const auto result =
        self->CreateCommandList(desc[2], static_cast<D3D12_COMMAND_LIST_TYPE>(desc[0]), allocator,
                                state, IID_PPV_ARGS(&host));
    *list = SUCCEEDED(result) ? make_list_proxy(host) : nullptr;
    return result;
}
// The uploads of placement textures the title wrote, on a list of ours that runs before the
// title's on the same queue. Textures are where the title keeps them between its own lists:
// shader resources.
struct Uploader {
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 value = 0;
    std::vector<ID3D12Resource *> buffers; // until the GPU is past them
};
std::mutex uploader_mutex;
void upload_placements_d3d12(ID3D12CommandQueue *queue) {
    // shader resource states are a direct queue's: the uploads wait for one
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    auto uploads = take_d3d12_uploads();
    if (uploads.empty())
        return;
    if (static std::atomic<int> shown{0}; std::getenv("XS_D3D_TRACE") && shown++ < 5) {
        std::fprintf(stderr,
                     "[Runtime] uploading %zu placement textures, the first with %zu "
                     "subresources\n",
                     uploads.size(), uploads[0].subresources.size());
        std::fflush(stderr);
    }
    const std::lock_guard lock(uploader_mutex);
    static Uploader uploader;
    ID3D12Device *device = nullptr;
    queue->GetDevice(IID_PPV_ARGS(&device));
    if (!uploader.list &&
        (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&uploader.allocator))) ||
         FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploader.allocator,
                                          nullptr, IID_PPV_ARGS(&uploader.list))) ||
         FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploader.fence))))) {
        device->Release();
        return;
    }
    if (uploader.value) {
        if (uploader.fence->GetCompletedValue() < uploader.value) {
            auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            uploader.fence->SetEventOnCompletion(uploader.value, event);
            WaitForSingleObject(event, 5000);
            CloseHandle(event);
        }
        for (auto *buffer : uploader.buffers)
            buffer->Release();
        uploader.buffers.clear();
        uploader.allocator->Reset();
        uploader.list->Reset(uploader.allocator, nullptr);
    }
    for (auto &upload : uploads) {
        const auto desc = upload.resource->GetDesc();
        const auto count = static_cast<UINT>(upload.subresources.size());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        std::vector<UINT> rows(count);
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(), nullptr,
                                      &bytes);
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                              0,
                                              bytes,
                                              1,
                                              1,
                                              1,
                                              DXGI_FORMAT_UNKNOWN,
                                              {1, 0},
                                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                              D3D12_RESOURCE_FLAG_NONE};
        ID3D12Resource *buffer = nullptr;
        void *mapped = nullptr;
        if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&buffer))) &&
            SUCCEEDED(buffer->Map(0, nullptr, &mapped))) {
            for (UINT i = 0; i < count; ++i) {
                const auto &source = upload.subresources[i];
                const auto pitch = upload.pitches[i];
                for (UINT row = 0; row < rows[i] && std::size_t{row + 1} * pitch <= source.size();
                     ++row)
                    std::memcpy(static_cast<std::uint8_t *>(mapped) + footprints[i].Offset +
                                    std::size_t{row} * footprints[i].Footprint.RowPitch,
                                source.data() + std::size_t{row} * pitch, pitch);
            }
            buffer->Unmap(0, nullptr);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Transition = {upload.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_COPY_DEST};
            uploader.list->ResourceBarrier(1, &barrier);
            for (UINT i = 0; i < count; ++i) {
                const D3D12_TEXTURE_COPY_LOCATION target{upload.resource,
                                                         D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                                         {.SubresourceIndex = i}};
                const D3D12_TEXTURE_COPY_LOCATION source{buffer,
                                                         D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                         {.PlacedFootprint = footprints[i]}};
                uploader.list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            }
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            uploader.list->ResourceBarrier(1, &barrier);
            uploader.buffers.push_back(buffer);
        } else if (buffer) {
            buffer->Release();
        }
        upload.resource->Release();
    }
    uploader.list->Close();
    ID3D12CommandList *const lists[] = {uploader.list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(uploader.fence, ++uploader.value);
    device->Release();
}
// ID3D12CommandQueue::ExecuteCommandLists: the host's lists for the title's proxies
void STDMETHODCALLTYPE execute_command_lists(ID3D12CommandQueue *self, UINT count,
                                             ID3D12CommandList *const *lists) {
    if (const auto *text = std::getenv("XS_D3D_SLOW"))
        Sleep(std::atoi(text));
    decode_written_heaps();
    upload_placements_d3d12(self);
    std::vector<ID3D12CommandList *> host(count);
    for (UINT i = 0; i < count; ++i)
        host[i] = host_list(lists[i]);
    self->ExecuteCommandLists(count, host.data());
    // XS_D3D_SYNC: the GPU done with this work before the title goes on, to tell its waits
    // from ours
    if (std::getenv("XS_D3D_SYNC")) {
        static ID3D12Fence *fence = nullptr;
        static UINT64 value = 0;
        if (!fence) {
            ID3D12Device *device = nullptr;
            self->GetDevice(IID_PPV_ARGS(&device));
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            device->Release();
        }
        self->Signal(fence, ++value);
        auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, 5000);
        CloseHandle(event);
    }
}
// CreateCommandList (12), the desktop's way: a proxy as well
HRESULT STDMETHODCALLTYPE create_command_list_desktop(ID3D12Device *self, UINT node,
                                                      D3D12_COMMAND_LIST_TYPE type,
                                                      ID3D12CommandAllocator *allocator,
                                                      ID3D12PipelineState *state, REFIID,
                                                      void **list) {
    if (!list)
        return E_POINTER;
    List *host = nullptr;
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, UINT, D3D12_COMMAND_LIST_TYPE,
                                                ID3D12CommandAllocator *, ID3D12PipelineState *,
                                                REFIID, void **);
    const auto result =
        original<Create>(self, 12)(self, node, type, allocator, state, IID_PPV_ARGS(&host));
    *list = SUCCEEDED(result) ? make_list_proxy(host) : nullptr;
    return result;
}
// The console's ID3D12Device goes on past the desktop's 44 methods with its own. Those not
// known yet say which they are and fail.
template <std::size_t Index>
HRESULT STDMETHODCALLTYPE unknown_device_method(void *, void *first, void *second) {
    std::fprintf(stderr,
                 "[Runtime] unimplemented Direct3D 12 call: device slot %zu (%p, %p) from %s\n",
                 Index, first, second, detail::guest_location(_ReturnAddress()).c_str());
    std::fflush(stderr);
    return E_NOTIMPL;
}
// CreatePlacedResource (29) and CreateReservedResource (30): the console's states and flags
D3D12_RESOURCE_DESC host_resource_desc(const D3D12_RESOURCE_DESC &desc) {
    auto host = desc;
    host.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host.Flags & 0xff);
    if (host.Layout >= 0x100)
        host.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return host;
}
HRESULT STDMETHODCALLTYPE create_placed_resource(ID3D12Device *self, ID3D12Heap *heap,
                                                 UINT64 offset, const D3D12_RESOURCE_DESC *desc,
                                                 D3D12_RESOURCE_STATES state,
                                                 const D3D12_CLEAR_VALUE *clear, REFIID iid,
                                                 void **resource) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(
        void *, ID3D12Heap *, UINT64, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
        const D3D12_CLEAR_VALUE *, REFIID, void **);
    D3D12_RESOURCE_DESC host{};
    if (desc)
        host = host_resource_desc(*desc);
    const auto result = original<Create>(self, 29)(self, heap, offset, desc ? &host : nullptr,
                                                   host_state(state), clear, iid, resource);
    if (SUCCEEDED(result) && resource && *resource)
        track_buffer(static_cast<ID3D12Resource *>(*resource));
    return result;
}
HRESULT STDMETHODCALLTYPE create_reserved_resource(ID3D12Device *self,
                                                   const D3D12_RESOURCE_DESC *desc,
                                                   D3D12_RESOURCE_STATES state,
                                                   const D3D12_CLEAR_VALUE *clear, REFIID iid,
                                                   void **resource) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
                                     const D3D12_CLEAR_VALUE *, REFIID, void **);
    D3D12_RESOURCE_DESC host{};
    if (desc)
        host = host_resource_desc(*desc);
    return original<Create>(self, 30)(self, desc ? &host : nullptr, host_state(state), clear, iid,
                                      resource);
}
// slot 60: CreateGraphicsPipelineStateX, the pipeline and the console's extensions of it
HRESULT STDMETHODCALLTYPE create_graphics_pipeline_x(ID3D12Device *self,
                                                     const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                                     UINT, const void *, REFIID iid, void **state) {
    return create_graphics_pipeline(self, desc, iid, state);
}
// what only the console's driver and debugger have a use for: driver hints, hang and debug
// callbacks, debug flags, memory priority, UnregisterPagePoolX
HRESULT STDMETHODCALLTYPE ignored_device_method(void *) { return S_OK; }
constexpr std::size_t host_device_slots = 44, device_slots = 128;
template <std::size_t... Index>
std::vector<std::pair<std::size_t, Slot>> unknown_device_methods(std::index_sequence<Index...>) {
    return {{host_device_slots + Index,
             reinterpret_cast<Slot>(&unknown_device_method<host_device_slots + Index>)}...};
}

// ID3D12Device8::CreateCommittedResource2 and CreateSamplerFeedbackUnorderedAccessView (the
// host's 69 and 71). The first Series kit numbers the two sampler feedback formats 187 and
// 188, which the desktop has at 189 and 190.
HRESULT STDMETHODCALLTYPE create_committed_resource_2(
    ID3D12Device *self, const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
    void *session, REFIID iid, void **resource) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(
        void *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1 *,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, void *, REFIID, void **);
    D3D12_RESOURCE_DESC1 host_desc{};
    if (desc) {
        host_desc = *desc;
        host_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host_desc.Flags & 0xff);
        if (host_desc.Layout >= 0x100)
            host_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (host_desc.Format == 187 || host_desc.Format == 188) {
            host_desc.Format = static_cast<DXGI_FORMAT>(host_desc.Format + 2);
            host_desc.Alignment = 0; // the host has no small alignment for these
            // The console takes a mip region as large as the texture, one value for all of
            // it. The host stops at half: its map is then 2x2 where the title expects 1x1.
            auto &region = host_desc.SamplerFeedbackMipRegion;
            region.Width = std::min<UINT>(region.Width, static_cast<UINT>(host_desc.Width / 2));
            region.Height = std::min(region.Height, host_desc.Height / 2);
            if (host_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                region.Depth = 0;
            // The host wants as many mips as the texture the map is paired with later, where
            // the console takes one: a full chain, which is what a texture worth feedback has
            if (host_desc.MipLevels == 1)
                host_desc.MipLevels = static_cast<UINT16>(
                    std::bit_width(std::max<std::uint64_t>(host_desc.Width, host_desc.Height)));
        }
    }
    if (state == static_cast<D3D12_RESOURCE_STATES>(0x8c3))
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    const auto result = original<Create>(self, 69)(self, heap, flags, desc ? &host_desc : nullptr,
                                                   state, clear, session, iid, resource);
    if (SUCCEEDED(result) && resource && *resource)
        console_resource(*resource);
    if (FAILED(result) && desc) {
        std::fprintf(stderr,
                     "[Runtime] CreateCommittedResource2 dimension %d %llux%ux%u mips %u format "
                     "%d flags 0x%x state 0x%x: 0x%08lx\n",
                     desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize,
                     desc->MipLevels, desc->Format, desc->Flags, state,
                     static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// The console's takes a view description as well, null for the whole map
void STDMETHODCALLTYPE create_feedback_view(ID3D12Device *self, ID3D12Resource *target,
                                            ID3D12Resource *feedback,
                                            const D3D12_UNORDERED_ACCESS_VIEW_DESC *,
                                            D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    using Create = void(STDMETHODCALLTYPE *)(void *, ID3D12Resource *, ID3D12Resource *,
                                             D3D12_CPU_DESCRIPTOR_HANDLE);
    original<Create>(self, 71)(self, target, feedback, host_handle(handle));
}
// ID3D12Device2::CreatePipelineState, from a stream of subobjects: the host's (47), and why
// when it turns the pipeline down
HRESULT STDMETHODCALLTYPE create_pipeline_state(ID3D12Device *self,
                                                const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                                                REFIID iid, void **state) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_PIPELINE_STATE_STREAM_DESC *,
                                                REFIID, void **);
    // The stream is subobjects one after another, each a type and its description, aligned
    // to 8 bytes. The Series kit numbers the amplification and mesh shaders 23 and 24, where
    // the desktop has them at 24 and 25, and has subobjects of its own from 25 on.
    static constexpr std::size_t sizes[] = {16, 24, 24, 24, 24, 24, 24, 40, 336, 8,  48, 56, 24,
                                            8,  8,  40, 8,  16, 8,  24, 8,  64,  32, 24, 24};
    std::vector<std::uint8_t> stream;
    D3D12_PIPELINE_STATE_STREAM_DESC host{};
    if (desc && desc->pPipelineStateSubobjectStream && console == Target::scarlett) {
        const auto *bytes = static_cast<const std::uint8_t *>(desc->pPipelineStateSubobjectStream);
        for (std::size_t at = 0; at + 8 <= desc->SizeInBytes;) {
            std::uint32_t type;
            std::memcpy(&type, bytes + at, 4);
            if (type >= std::size(sizes)) {
                if (static std::atomic<bool> shown{false}; !shown.exchange(true))
                    std::fprintf(stderr,
                                 "[Runtime] CreatePipelineState: console subobject %u, which "
                                 "ends what the host is given of the stream\n",
                                 type);
                break;
            }
            const auto size = std::min(sizes[type], desc->SizeInBytes - at);
            stream.insert(stream.end(), bytes + at, bytes + at + size);
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY && size >= 8) {
                D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
                std::memcpy(&topology, stream.data() + stream.size() - size + 4, 4);
                topology = host_topology_type(topology);
                std::memcpy(stream.data() + stream.size() - size + 4, &topology, 4);
            }
            if (type == 23 || type == 24) {
                ++type;
                std::memcpy(stream.data() + stream.size() - size, &type, 4);
            }
            at += size;
        }
        host = {stream.size(), stream.data()};
        desc = &host;
    }
    const auto result = original<Create>(self, 47)(self, desc, iid, state);
    if (FAILED(result)) {
        std::fprintf(stderr, "[Runtime] CreatePipelineState from a stream of %zu bytes: 0x%08lx\n",
                     desc ? desc->SizeInBytes : 0, static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// the host's method `Index` after its first 44, at whatever slot the console has it
template <std::size_t... Index>
std::array<Slot, sizeof...(Index)> later_device_methods(std::index_sequence<Index...>) {
    return {reinterpret_cast<Slot>(&moved_method<host_device_slots + Index>)...};
}

// D3D12XBOX_CREATE_DEVICE_PARAMETERS sizes the console's rings, which mean nothing here
HRESULT WINAPI xbox_create_device(void *, const void *, REFIID iid, void **device) {
    if (!device)
        return E_POINTER;
    *device = nullptr;
    configure_lists();
    using Create = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateDevice");
    if (std::getenv("XS_D3D_DEBUG")) {
        using GetDebug = HRESULT(WINAPI *)(REFIID, void **);
        static const auto get_debug = host_d3d12<GetDebug>("D3D12GetDebugInterface");
        if (ID3D12Debug *debug = nullptr; get_debug && SUCCEEDED(get_debug(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            // XS_D3D_DEBUG=2: also what each draw reads, checked on the GPU (slow)
            if (ID3D12Debug1 *gpu = nullptr; std::atoi(std::getenv("XS_D3D_DEBUG")) >= 2 &&
                                             SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&gpu)))) {
                // 3: the queues not serialized, as they are without the debug layer
                if (std::atoi(std::getenv("XS_D3D_DEBUG")) == 3)
                    gpu->SetEnableSynchronizedCommandQueueValidation(FALSE);
                else
                    gpu->SetEnableGPUBasedValidation(TRUE);
                gpu->Release();
            }
            debug->Release();
        }
        // what the GPU was doing when the device goes, and where it faulted
        if (ID3D12DeviceRemovedExtendedDataSettings *dred = nullptr;
            get_debug && SUCCEEDED(get_debug(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->Release();
        }
    }
    ID3D12Device *host = nullptr;
    const auto result =
        create ? create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host)) : E_NOTIMPL;
    if (SUCCEEDED(result))
        d3d12_device = host;
    if (ID3D12InfoQueue *queue = nullptr;
        SUCCEEDED(result) && SUCCEEDED(host->QueryInterface(IID_PPV_ARGS(&queue))))
        debug_messages = queue;
    std::fprintf(stderr, "[Runtime] Direct3D 12 device for an %s title on the host: 0x%08lx\n",
                 std::string(target_name(console)).c_str(), static_cast<unsigned long>(result));
    std::fflush(stderr);
    if (FAILED(result))
        return result;
    auto overrides =
        unknown_device_methods(std::make_index_sequence<device_slots - host_device_slots>{});
    overrides.push_back({0, reinterpret_cast<Slot>(&device_query)});
    overrides.push_back({13, reinterpret_cast<Slot>(&check_feature_support)});
    overrides.push_back({14, reinterpret_cast<Slot>(&create_descriptor_heap)});
    overrides.push_back({15, reinterpret_cast<Slot>(&descriptor_increment)});
    overrides.push_back({16, reinterpret_cast<Slot>(&create_root_signature)});
    overrides.push_back({17, reinterpret_cast<Slot>(&create_cbv)});
    overrides.push_back({18, reinterpret_cast<Slot>(&create_srv)});
    overrides.push_back({19, reinterpret_cast<Slot>(&create_uav)});
    overrides.push_back({20, reinterpret_cast<Slot>(&create_target_view<20>)});
    overrides.push_back({21, reinterpret_cast<Slot>(&create_target_view<21>)});
    overrides.push_back({22, reinterpret_cast<Slot>(&create_sampler)});
    overrides.push_back({23, reinterpret_cast<Slot>(&copy_descriptors)});
    overrides.push_back({24, reinterpret_cast<Slot>(&copy_descriptors_simple)});
    overrides.push_back({25, reinterpret_cast<Slot>(&resource_allocation_info)});
    // GetAdapterLuid returns its 8 bytes in RAX
    overrides.push_back({43, reinterpret_cast<Slot>(&returned_in_rax<43>)});
    overrides.push_back({26, reinterpret_cast<Slot>(&custom_heap_properties)});
    overrides.push_back({27, reinterpret_cast<Slot>(&create_committed_resource)});
    overrides.push_back({44, reinterpret_cast<Slot>(&create_placed_resource_x)});
    overrides.push_back({8, reinterpret_cast<Slot>(&create_command_queue)});
    overrides.push_back({62, reinterpret_cast<Slot>(&create_compute_pipeline_x)});
    overrides.push_back({74, reinterpret_cast<Slot>(&register_page_pool)});
    overrides.push_back({10, reinterpret_cast<Slot>(&create_graphics_pipeline)});
    overrides.push_back({11, reinterpret_cast<Slot>(&create_compute_pipeline)});
    overrides.push_back({48, reinterpret_cast<Slot>(&create_command_queue)});
    overrides.push_back({73, reinterpret_cast<Slot>(&create_command_list)});
    overrides.push_back({12, reinterpret_cast<Slot>(&create_command_list_desktop)});
    overrides.push_back({29, reinterpret_cast<Slot>(&create_placed_resource)});
    overrides.push_back({30, reinterpret_cast<Slot>(&create_reserved_resource)});
    overrides.push_back({60, reinterpret_cast<Slot>(&create_graphics_pipeline_x)});
    for (const std::size_t ignored : {49, 50, 51, 52, 53, 54, 58, 69, 72, 75})
        overrides.push_back({ignored, reinterpret_cast<Slot>(&ignored_device_method)});
    // how a GDK title paces its frames: there is no swap chain to wait for
    overrides.push_back({78, reinterpret_cast<Slot>(&set_frame_interval)});
    overrides.push_back({79, reinterpret_cast<Slot>(&schedule_frame_event)});
    overrides.push_back({80, reinterpret_cast<Slot>(&wait_frame_event)});
    overrides.push_back({81, reinterpret_cast<Slot>(&frame_statistics)});
    // The later device interfaces follow the console's own methods, where the host has them
    // right after the first interface's 44: ID3D12Device1 to 3 on Xbox One (87 to 92), and up
    // to ID3D12Device8 on Xbox Series (83 to 112, with a console method at 104).
    const auto later = later_device_methods(std::make_index_sequence<32>{});
    if (console == Target::durango) {
        for (std::size_t slot = 87; slot <= 92; ++slot)
            overrides.push_back({slot, later[slot - 87]});
        overrides.push_back({90, reinterpret_cast<Slot>(&create_pipeline_state)});
    } else {
        for (std::size_t slot = 83; slot <= 103; ++slot)
            overrides.push_back({slot, later[slot - 83]});
        for (std::size_t slot = 105; slot <= 112; ++slot)
            overrides.push_back({slot, later[slot - 84]});
        overrides.push_back({86, reinterpret_cast<Slot>(&create_pipeline_state)});
        overrides.push_back({109, reinterpret_cast<Slot>(&create_committed_resource_2)});
        overrides.push_back({111, reinterpret_cast<Slot>(&create_feedback_view)});
    }
    // all of the host's vtable, which its runtime calls past the desktop's first 44 too
    console_vtable<Family::device>(host, 96, device_slots, overrides);
    // through the console's QueryInterface, which knows the kits' interface IDs
    const auto queried = device_query(host, iid, device);
    host->Release();
    return queried;
}
} // namespace

void console_d3d12_resource(void *resource) { console_resource(resource); }
void report_failure() {
    auto *const device = d3d12_device.load();
    if (!device)
        return;
    {
        const std::lock_guard lock(signature_mutex);
        std::fprintf(stderr,
                     "[Runtime] root signatures %zu; title descriptors read by their tables: SRV "
                     "%llu, UAV %llu, CBV %llu, sampler %llu\n",
                     tables_of.size(), static_cast<unsigned long long>(decoded[0].load()),
                     static_cast<unsigned long long>(decoded[1].load()),
                     static_cast<unsigned long long>(decoded[2].load()),
                     static_cast<unsigned long long>(decoded[3].load()));
    }
    std::fprintf(stderr, "[Runtime] Direct3D 12 device removed: 0x%08lx\n",
                 static_cast<unsigned long>(device->GetDeviceRemovedReason()));
    report_messages();
    ID3D12DeviceRemovedExtendedData *dred = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
        return;
    if (D3D12_DRED_PAGE_FAULT_OUTPUT fault{}; SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)))
        std::fprintf(stderr, "[Runtime] GPU page fault at 0x%llx\n",
                     static_cast<unsigned long long>(fault.PageFaultVA));
    if (D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
        SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
        for (auto *node = crumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
            const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            if (done >= node->BreadcrumbCount)
                continue;
            std::fprintf(stderr, "[Runtime] GPU stopped after %u of %u operations:", done,
                         node->BreadcrumbCount);
            for (UINT i = done > 3 ? done - 3 : 0; i < std::min(done + 3, node->BreadcrumbCount);
                 ++i)
                std::fprintf(stderr, " %s%d", i == done ? ">" : "",
                             static_cast<int>(node->pCommandHistory[i]));
            std::fprintf(stderr, "\n");
        }
    dred->Release();
    std::fflush(stderr);
}
ID3D12Resource *last_render_target() { return last_target.load(); }
std::uint64_t draws_into_target(ID3D12Resource *target) {
    const std::lock_guard lock(draw_mutex);
    const auto found = draws_into.find(target);
    return found == draws_into.end() ? 0 : found->second;
}
std::vector<ID3D12Resource *> bound_render_targets() {
    const std::lock_guard lock(target_mutex);
    return bound_targets;
}
bool read_back(ID3D12Resource *resource, std::vector<std::uint8_t> &data,
                             unsigned &pitch, unsigned &width, unsigned &height,
                             DXGI_FORMAT &format, D3D12_RESOURCE_STATES state) {
    auto *const queue = first_direct_queue.load();
    ID3D12Device *device = nullptr;
    if (!queue || FAILED(resource->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    const auto desc = resource->GetDesc();
    // a multisampled texture cannot be copied to a buffer
    if (desc.SampleDesc.Count > 1) {
        device->Release();
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                    0,
                                    bytes,
                                    1,
                                    1,
                                    1,
                                    DXGI_FORMAT_UNKNOWN,
                                    {1, 0},
                                    D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                    D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *buffer = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    bool done = false;
    if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&buffer))) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                            IID_PPV_ARGS(&list))) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Transition = {resource, 0, state, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        const D3D12_TEXTURE_COPY_LOCATION target{
            buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
        const D3D12_TEXTURE_COPY_LOCATION source{
            resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}};
        list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList *const lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, 1);
        auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, 5000);
        CloseHandle(event);
        void *mapped = nullptr;
        if (SUCCEEDED(buffer->Map(0, nullptr, &mapped))) {
            const auto *start = static_cast<const std::uint8_t *>(mapped) + footprint.Offset;
            data.assign(start, start + std::size_t{footprint.Footprint.RowPitch} *
                                           footprint.Footprint.Height);
            buffer->Unmap(0, nullptr);
            pitch = footprint.Footprint.RowPitch;
            width = footprint.Footprint.Width;
            height = footprint.Footprint.Height;
            format = desc.Format;
            done = true;
        }
    }
    for (IUnknown *object : {static_cast<IUnknown *>(fence), static_cast<IUnknown *>(list),
                             static_cast<IUnknown *>(allocator), static_cast<IUnknown *>(buffer),
                             static_cast<IUnknown *>(device)})
        if (object)
            object->Release();
    return done;
}
ID3D12CommandQueue *direct_queue() { return first_direct_queue.load(); }
D3D12_RESOURCE_STATES tracked_state(ID3D12Resource *resource, D3D12_RESOURCE_STATES unknown) {
    const std::lock_guard lock(state_mutex);
    const auto found = target_states.find(resource);
    return found == target_states.end() ? unknown : found->second;
}
void set_adapter_wrapper(AdapterWrapper wrapper) { adapter_wrapper = wrapper; }

namespace {
HRESULT WINAPI succeeded() { return S_OK; }
// the console's debug interface is its own; titles go on without one
HRESULT WINAPI no_debug_interface(REFIID, void **debug) {
    if (debug)
        *debug = nullptr;
    return E_NOINTERFACE;
}
// Interface IDs the DLL exports as data, for titles built without their own copies
constexpr GUID graphics_unknown{
    0xaceeea63, 0xe0a9, 0x4a1c, {0xbb, 0xec, 0x71, 0xb2, 0xf4, 0x85, 0xf7, 0x58}};
constexpr GUID dma_command_list{
    0x30bc77b9, 0x8379, 0x4b25, {0x95, 0xde, 0xb8, 0x58, 0x8c, 0x42, 0x27, 0x5d}};
constexpr GUID debug_object_name{
    0x4cca5fd8, 0x921f, 0x42c8, {0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41}};
template <class Interface> void *iid_of() {
    static const GUID iid = __uuidof(Interface);
    return const_cast<GUID *>(&iid);
}
} // namespace

void install(Registry &registry, std::string_view module, Target target) {
    console = target;
    const std::pair<const char *, void *> functions[] = {
        {"D3D12XboxCreateDevice", reinterpret_cast<void *>(&xbox_create_device)},
        {"D3D12CreateDevice", reinterpret_cast<void *>(&create_device_12)},
        {"D3D12GetDebugInterface", reinterpret_cast<void *>(&no_debug_interface)},
        {"D3D12XboxSetProcessDebugFlags", reinterpret_cast<void *>(&succeeded)},
        {"D3D12XboxGetProcessDebugFlags", reinterpret_cast<void *>(&succeeded)},
        {"D3D12XboxEnableMemoryProfiling", reinterpret_cast<void *>(&succeeded)},
        {"D3D12XboxUnloadDriverModule", reinterpret_cast<void *>(&succeeded)},
        {"D3D12ConfigureVirtualMemoryX", reinterpret_cast<void *>(&succeeded)},
        {"D3DConfigureVirtualMemory", reinterpret_cast<void *>(&succeeded)},
        {"D3DFlushEntireCpuCache", reinterpret_cast<void *>(&succeeded)},
        {"D3D12CreateRootSignatureDeserializer",
         reinterpret_cast<void *>(&root_signature_deserializer)},
        {"D3D12CreateVersionedRootSignatureDeserializer",
         reinterpret_cast<void *>(&versioned_root_signature_deserializer)},
        {"D3D12SerializeRootSignature", reinterpret_cast<void *>(&serialize_root_signature)},
        {"D3D12SerializeVersionedRootSignature",
         reinterpret_cast<void *>(&serialize_versioned_root_signature)},
        {"IID_IGraphicsUnknown", const_cast<GUID *>(&graphics_unknown)},
        {"IID_ID3D12XboxDmaCommandList", const_cast<GUID *>(&dma_command_list)},
        {"WKPDID_D3DDebugObjectNameW", const_cast<GUID *>(&debug_object_name)},
        {"IID_ID3D12CommandAllocator", iid_of<ID3D12CommandAllocator>()},
        {"IID_ID3D12CommandList", iid_of<ID3D12CommandList>()},
        {"IID_ID3D12CommandQueue", iid_of<ID3D12CommandQueue>()},
        {"IID_ID3D12Debug", iid_of<ID3D12Debug>()},
        {"IID_ID3D12DescriptorHeap", iid_of<ID3D12DescriptorHeap>()},
        {"IID_ID3D12Device", iid_of<ID3D12Device>()},
        {"IID_ID3D12DeviceChild", iid_of<ID3D12DeviceChild>()},
        {"IID_ID3D12Fence", iid_of<ID3D12Fence>()},
        {"IID_ID3D12GraphicsCommandList", iid_of<ID3D12GraphicsCommandList>()},
        {"IID_ID3D12Heap", iid_of<ID3D12Heap>()},
        {"IID_ID3D12Object", iid_of<ID3D12Object>()},
        {"IID_ID3D12Pageable", iid_of<ID3D12Pageable>()},
        {"IID_ID3D12PipelineState", iid_of<ID3D12PipelineState>()},
        {"IID_ID3D12Resource", iid_of<ID3D12Resource>()},
        {"IID_ID3D12RootSignature", iid_of<ID3D12RootSignature>()},
        {"IID_ID3D12RootSignatureDeserializer", iid_of<ID3D12RootSignatureDeserializer>()},
        {"IID_IDXGIAdapter", iid_of<IDXGIAdapter>()},
        {"IID_IDXGIAdapter1", iid_of<IDXGIAdapter1>()},
        {"IID_IDXGIAdapter2", iid_of<IDXGIAdapter2>()},
        {"IID_IDXGIDevice", iid_of<IDXGIDevice>()},
        {"IID_IDXGIDevice1", iid_of<IDXGIDevice1>()},
        {"IID_IDXGIDevice2", iid_of<IDXGIDevice2>()},
        {"IID_IDXGIDeviceSubObject", iid_of<IDXGIDeviceSubObject>()},
        {"IID_IDXGIFactory", iid_of<IDXGIFactory>()},
        {"IID_IDXGIFactory1", iid_of<IDXGIFactory1>()},
        {"IID_IDXGIFactory2", iid_of<IDXGIFactory2>()},
        {"IID_IDXGIObject", iid_of<IDXGIObject>()},
        {"IID_IDXGISwapChain", iid_of<IDXGISwapChain>()},
        {"IID_IDXGISwapChain1", iid_of<IDXGISwapChain1>()}};
    for (const auto &[name, function] : functions)
        registry.add(module, name, function);
}
} // namespace xs::runtime::modules::d3d12x
