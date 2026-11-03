#include "backends/graphics/backend.hpp"
#include "backends/graphics/d3d11/host.hpp"
#include "backends/graphics/dxvk/host.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/common/com_forward.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/d3d11x/layout.hpp"
#include "runtime/durango/d3d11x/wrap.hpp"
#include "runtime/durango/runtime.hpp"
#include "runtime/durango/winrt/application.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <wrl/implements.h>

// d3d11_x.dll on the host's Direct3D 11. Direct3D objects go to the title wrapped in the
// console layout (wrap.hpp). The DXGI device, adapter and factory are the same as on the
// desktop and are passed through, except for the way to the swap chain: a console swap
// chain is made for a CoreWindow, and ours is not one the host would accept.
namespace xs::runtime::durango {
namespace d3d11x {
HRESULT device_dxgi(IUnknown *host_device, REFIID iid, void **object);
}
namespace {
template <class... Interfaces> bool is_any(REFIID iid) {
    return ((iid == __uuidof(Interfaces)) || ...);
}
HRESULT keep(Forwarder *self, void **object) {
    forwarder_add_ref(self);
    *object = self;
    return S_OK;
}
void *slot(auto *function) { return reinterpret_cast<void *>(function); }

// IDXGIFactory2::CreateSwapChainForCoreWindow
// The title's first direct queue: a console Direct3D 12 swap chain is made for the device,
// the host's for the queue that presents
std::atomic<ID3D12CommandQueue *> direct_queue{nullptr};
HRESULT STDMETHODCALLTYPE create_swap_chain(Forwarder *self, IUnknown *device, IUnknown *,
                                            const DXGI_SWAP_CHAIN_DESC1 *description, IDXGIOutput *,
                                            IDXGISwapChain1 **swap_chain) {
    auto *const window = winrt::current_title_window();
    if (!window || !description || !swap_chain)
        return E_INVALIDARG;
    auto *const factory = static_cast<IDXGIFactory2 *>(forwarder_host(self));
    // what a CoreWindow takes and a window handle does not
    auto host = *description;
    // flip swap chains take no typeless formats, which the console's do
    if (host.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS)
        host.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    else if (host.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
        host.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (host.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
        host.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    else if (host.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS)
        host.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (host.Scaling == DXGI_SCALING_ASPECT_RATIO_STRETCH)
        host.Scaling = DXGI_SCALING_STRETCH;
    if (host.AlphaMode != DXGI_ALPHA_MODE_UNSPECIFIED && host.AlphaMode != DXGI_ALPHA_MODE_IGNORE)
        host.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    const bool flip = host.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                      host.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (flip) {
        host.BufferCount = std::max(host.BufferCount, 2u);
        host.SampleDesc = {1, 0};
    }
    host.Flags &= DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE |
                  DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                  DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    auto *presenter = static_cast<IUnknown *>(d3d11x::unwrap(device));
    if (ID3D12Device *d3d12 = nullptr;
        presenter && SUCCEEDED(presenter->QueryInterface(IID_PPV_ARGS(&d3d12)))) {
        d3d12->Release();
        if (auto *const queue = direct_queue.load())
            presenter = queue;
    }
    const auto result = factory->CreateSwapChainForHwnd(
        presenter, static_cast<HWND>(window->native()), &host, nullptr, nullptr, swap_chain);
    if (SUCCEEDED(result))
        *swap_chain =
            static_cast<IDXGISwapChain1 *>(d3d11x::wrap(*swap_chain, d3d11x::Kind::swap_chain));
    std::fprintf(
        stderr,
        "[Runtime] swap chain %ux%u format %u buffers %u effect %u scaling %u alpha %u "
        "samples %u usage 0x%x flags 0x%x for the title window: 0x%08lx\n",
        description->Width, description->Height, static_cast<unsigned>(description->Format),
        description->BufferCount, static_cast<unsigned>(description->SwapEffect),
        static_cast<unsigned>(description->Scaling), static_cast<unsigned>(description->AlphaMode),
        description->SampleDesc.Count, static_cast<unsigned>(description->BufferUsage),
        description->Flags, static_cast<unsigned long>(result));
    std::fflush(stderr);
    return result;
}
// the host's factory up to IDXGIFactory7 where it has it, 32 methods, else IDXGIFactory2
bool latest_factory = false;
HRESULT factory_query(Forwarder *self, REFIID iid, void **object) {
    if (is_any<IUnknown, IDXGIObject, IDXGIFactory, IDXGIFactory1, IDXGIFactory2>(iid) ||
        (latest_factory &&
         is_any<IDXGIFactory3, IDXGIFactory4, IDXGIFactory5, IDXGIFactory6, IDXGIFactory7>(iid)))
        return keep(self, object);
    return forwarder_host(self)->QueryInterface(iid, object);
}
void *factory_forwarder(IDXGIFactory2 *factory) {
    IDXGIFactory7 *latest = nullptr;
    latest_factory = SUCCEEDED(factory->QueryInterface(&latest));
    if (latest_factory)
        factory->Release();
    return make_forwarder(latest_factory ? latest : factory, latest_factory ? 32 : 25,
                          &factory_query, {{16, slot(&create_swap_chain)}});
}

// IDXGIObject::GetParent of the adapter
HRESULT STDMETHODCALLTYPE adapter_parent(Forwarder *self, REFIID iid, void **parent) {
    auto *const adapter = static_cast<IDXGIAdapter *>(forwarder_host(self));
    if (!is_any<IDXGIFactory, IDXGIFactory1, IDXGIFactory2, IDXGIFactory3, IDXGIFactory4,
                IDXGIFactory5, IDXGIFactory6, IDXGIFactory7>(iid))
        return adapter->GetParent(iid, parent);
    IDXGIFactory2 *factory = nullptr;
    const auto result =
        adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory));
    if (FAILED(result))
        return result;
    *parent = factory_forwarder(factory);
    return S_OK;
}
bool latest_adapter = false;
HRESULT adapter_query(Forwarder *self, REFIID iid, void **object) {
    if (is_any<IUnknown, IDXGIObject, IDXGIAdapter, IDXGIAdapter1, IDXGIAdapter2>(iid) ||
        (latest_adapter && is_any<IDXGIAdapter3, IDXGIAdapter4>(iid)))
        return keep(self, object);
    return forwarder_host(self)->QueryInterface(iid, object);
}

// The host's adapter, with GetParent leading to our factory; takes over `host`
HRESULT wrap_adapter(IDXGIAdapter *host, IDXGIAdapter **adapter) {
    HRESULT result = S_OK;
    // up to IDXGIAdapter4 where the host has it, 19 methods, else IDXGIAdapter2
    IDXGIAdapter4 *fourth = nullptr;
    latest_adapter = SUCCEEDED(host->QueryInterface(&fourth));
    IDXGIAdapter2 *latest = nullptr;
    if (latest_adapter)
        latest = fourth;
    else
        result = host->QueryInterface(&latest);
    host->Release();
    if (FAILED(result))
        return result;
    *adapter = reinterpret_cast<IDXGIAdapter *>(make_forwarder(
        latest, latest_adapter ? 19 : 12, &adapter_query, {{6, slot(&adapter_parent)}}));
    return S_OK;
}
// IDXGIDevice::GetAdapter
HRESULT STDMETHODCALLTYPE device_adapter(Forwarder *self, IDXGIAdapter **adapter) {
    IDXGIAdapter *host = nullptr;
    const auto result = static_cast<IDXGIDevice *>(forwarder_host(self))->GetAdapter(&host);
    return FAILED(result) ? result : wrap_adapter(host, adapter);
}
// IDXGIObject::GetParent of the DXGI device, the other way to its adapter
HRESULT STDMETHODCALLTYPE device_parent(Forwarder *self, REFIID iid, void **parent) {
    if (!is_any<IDXGIAdapter, IDXGIAdapter1, IDXGIAdapter2, IDXGIAdapter3, IDXGIAdapter4>(iid))
        return static_cast<IDXGIDevice *>(forwarder_host(self))->GetParent(iid, parent);
    return device_adapter(self, reinterpret_cast<IDXGIAdapter **>(parent));
}
HRESULT dxgi_device_query(Forwarder *self, REFIID iid, void **object) {
    if (is_any<IUnknown, IDXGIObject, IDXGIDevice, IDXGIDevice1>(iid))
        return keep(self, object);
    return forwarder_host(self)->QueryInterface(iid, object);
}

HRESULT WINAPI create_device(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT flags,
                             const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT,
                             ID3D11Device **device, D3D_FEATURE_LEVEL *level,
                             ID3D11DeviceContext **context) {
    if (device)
        *device = nullptr;
    if (context)
        *context = nullptr;
    const auto backend = graphics::selected_backend();
    if (!graphics::backend_renders_titles(backend)) {
        std::fprintf(stderr,
                     "[Runtime] the vulkan backend needs xs_d3d11.dll and xs_dxgi.dll next to "
                     "xscarlet.exe\n");
        std::fflush(stderr);
        return DXGI_ERROR_UNSUPPORTED;
    }
    // Console-only flag bits and the debug layer, which may not be installed, are dropped.
    const UINT host_flags =
        flags & (D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                 D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS);
    ID3D11Device1 *latest = nullptr;
    bool debug_layer = false;
    const bool vulkan = backend == graphics::Backend::vulkan;
    const auto result = vulkan ? graphics::dxvk::create_device(host_flags, levels, level_count,
                                                               &latest, level, context)
                               : graphics::d3d11::create_device(
                                     host_flags, std::getenv("XS_D3D_DEBUG") != nullptr, levels,
                                     level_count, &latest, level, context, debug_layer);
    if (debug_layer)
        d3d11x::watch_debug_messages(latest);
    std::fprintf(stderr, "[Runtime] Direct3D 11 device on %s (flags 0x%x): 0x%08lx\n",
                 vulkan ? "Vulkan" : "the host", flags, static_cast<unsigned long>(result));
    std::fflush(stderr);
    if (FAILED(result)) {
        if (context && *context) {
            (*context)->Release();
            *context = nullptr;
        }
        return result;
    }
    if (context && *context)
        *context =
            static_cast<ID3D11DeviceContext *>(d3d11x::wrap(*context, d3d11x::Kind::context));
    if (device)
        *device = static_cast<ID3D11Device *>(d3d11x::wrap(latest, d3d11x::Kind::device));
    else
        latest->Release();
    return S_OK;
}

// The parameters are the
// console's ring and tessellation buffer sizes, which mean nothing to the host.
HRESULT WINAPI create_device_x(const void *, ID3D11Device **device, ID3D11DeviceContext **context) {
    return create_device(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, device, nullptr, context);
}
HRESULT WINAPI present_array(UINT interval, UINT, UINT, UINT count, void **swap_chains,
                             const void *) {
    auto result = S_OK;
    for (UINT i = 0; swap_chains && i < count; ++i)
        if (const auto presented = d3d11x::present_swap_chain(swap_chains[i], interval);
            FAILED(presented))
            result = presented;
    return result;
}
// DXGIX_FRAME_STATISTICS, 0x70 bytes: nothing measured
HRESULT WINAPI frame_statistics(UINT count, void *statistics) {
    if (statistics)
        std::memset(statistics, 0, std::size_t{count} * 0x70);
    return S_OK;
}
// ESRAM is ordinary memory here, 64 KiB pages committed where the title reserved them
HRESULT WINAPI map_esram(UINT, void *address, UINT pages, const UINT *) {
    return commit_title_memory(address, std::size_t{pages} << 16) ? S_OK : E_OUTOFMEMORY;
}
HRESULT WINAPI ignored() { return S_OK; }

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
        return FAILED(result) ? result : wrap_adapter(host, adapter);
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
enum class Family { device, list, heap, queue };
constexpr std::size_t max_slots = 128;
template <Family Kind> std::array<Slot, max_slots> console_table{};
using Raw = std::uint64_t(STDMETHODCALLTYPE *)(void *, std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t);
template <Family Kind, std::size_t Index>
std::uint64_t STDMETHODCALLTYPE by_caller(void *first, std::uint64_t a, std::uint64_t b,
                                          std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                          std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                          std::uint64_t i, std::uint64_t j) {
    const auto method = detail::in_title(_ReturnAddress()) ? console_table<Kind>[Index]
                                                           : original_vtable(first)[Index];
    return reinterpret_cast<Raw>(method)(first, a, b, c, d, e, f, g, h, i, j);
}
template <Family Kind, std::size_t... Index>
std::array<Slot, max_slots> by_caller_slots(std::index_sequence<Index...>) {
    return {reinterpret_cast<Slot>(&by_caller<Kind, Index>)...};
}
template <Family Kind>
const auto dispatchers = by_caller_slots<Kind>(std::make_index_sequence<max_slots>{});
template <Family Kind>
void console_vtable(void *object, std::size_t host_slots, std::size_t slots,
                    const std::vector<std::pair<std::size_t, Slot>> &overrides) {
    auto *const host = *static_cast<Slot **>(object);
    auto *const block = new Slot[slots + 1]{};
    block[0] = host;
    std::copy(host, host + host_slots, block + 1);
    for (const auto &[index, function] : overrides) {
        console_table<Kind>[index] = function;
        block[1 + index] = dispatchers<Kind>[index];
    }
    *static_cast<Slot **>(object) = block + 1;
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
// ID3D12Device::QueryInterface: the console's device is also its DXGI device
HRESULT STDMETHODCALLTYPE device_query(ID3D12Device *self, REFIID iid, void **object) {
    if (object && is_any<IDXGIObject, IDXGIDevice, IDXGIDevice1>(iid)) {
        *object = static_cast<IDXGIDevice1 *>(
            Microsoft::WRL::Make<D3D12DxgiDevice>(self->GetAdapterLuid()).Detach());
        return S_OK;
    }
    return original<HRESULT(STDMETHODCALLTYPE *)(void *, REFIID, void **)>(self, 0)(self, iid,
                                                                                    object);
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
};
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
                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1);
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
        return {heap->host.ptr + (handle.ptr - heap->base) / heap->stride * heap->stride};
    return handle;
}
void mark_host(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    const std::lock_guard lock(shadow_mutex);
    if (shadow_heap(handle.ptr))
        std::memcpy(reinterpret_cast<void *>(handle.ptr), &host_mark, sizeof host_mark);
}
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
    if (first == host_mark || first == 0)
        return;
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
                     D3D12_CPU_DESCRIPTOR_HANDLE host) {
    const UINT stride = device->GetDescriptorHandleIncrementSize(desc.Type);
    auto *const memory = low_memory(std::size_t{desc.NumDescriptors} * stride);
    if (!memory) {
        std::fprintf(stderr, "[Runtime] no memory below 4 GiB for a descriptor heap\n");
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    const std::lock_guard lock(shadow_mutex);
    shadow_heaps[base] = {
        base, base + std::size_t{desc.NumDescriptors} * stride, host, stride, desc.Type, device};
    shadow_of[heap] = base;
}
// ID3D12DescriptorHeap::GetCPUDescriptorHandleForHeapStart: the shadow's start
std::uint64_t STDMETHODCALLTYPE cpu_heap_start(void *self) {
    {
        const std::lock_guard lock(shadow_mutex);
        if (const auto found = shadow_of.find(self); found != shadow_of.end())
            return found->second;
    }
    return returned_in_rax<9>(self);
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
template <std::size_t Index>
void STDMETHODCALLTYPE create_target_view(ID3D12Device *self, ID3D12Resource *resource,
                                          const void *desc, Handle handle) {
    original<void(STDMETHODCALLTYPE *)(void *, void *, const void *, Handle)>(self, Index)(
        self, resource, desc, host_handle(handle));
    mark_host(handle);
}
void STDMETHODCALLTYPE create_sampler(ID3D12Device *self, const D3D12_SAMPLER_DESC *desc,
                                      Handle handle) {
    original<void(STDMETHODCALLTYPE *)(void *, const void *, Handle)>(self, 22)(
        self, desc, host_handle(handle));
    mark_host(handle);
}
void STDMETHODCALLTYPE copy_descriptors(ID3D12Device *self, UINT destination_ranges,
                                        const Handle *destinations, const UINT *destination_sizes,
                                        UINT source_ranges, const Handle *sources,
                                        const UINT *source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    std::vector<Handle> host_destinations(destinations, destinations + destination_ranges);
    std::vector<Handle> host_sources(sources, sources + source_ranges);
    UINT stride = self->GetDescriptorHandleIncrementSize(type);
    for (UINT range = 0; range < source_ranges; ++range) {
        const UINT count = source_sizes ? source_sizes[range] : 1;
        for (UINT i = 0; i < count; ++i)
            decode_written({sources[range].ptr + std::size_t{i} * stride});
        host_sources[range] = host_handle(sources[range]);
    }
    for (UINT range = 0; range < destination_ranges; ++range) {
        const UINT count = destination_sizes ? destination_sizes[range] : 1;
        for (UINT i = 0; i < count; ++i)
            mark_host({destinations[range].ptr + std::size_t{i} * stride});
        host_destinations[range] = host_handle(destinations[range]);
    }
    using Copy =
        void(STDMETHODCALLTYPE *)(void *, UINT, const Handle *, const UINT *, UINT, const Handle *,
                                  const UINT *, D3D12_DESCRIPTOR_HEAP_TYPE);
    original<Copy>(self, 23)(self, destination_ranges, host_destinations.data(), destination_sizes,
                             source_ranges, host_sources.data(), source_sizes, type);
}
void STDMETHODCALLTYPE copy_descriptors_simple(ID3D12Device *self, UINT count, Handle destination,
                                               Handle source, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    const UINT stride = self->GetDescriptorHandleIncrementSize(type);
    for (UINT i = 0; i < count; ++i) {
        decode_written({source.ptr + std::size_t{i} * stride});
        mark_host({destination.ptr + std::size_t{i} * stride});
    }
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
    const auto host =
        static_cast<ID3D12DescriptorHeap *>(*heap)->GetCPUDescriptorHandleForHeapStart();
    console_vtable<Family::heap>(*heap, 11, 11,
                                 {{9, reinterpret_cast<Slot>(&cpu_heap_start)},
                                  {10, reinterpret_cast<Slot>(&returned_in_rax<10>)}});
    register_shadow(self, *heap, *desc, host);
    return result;
}
HRESULT STDMETHODCALLTYPE create_committed_resource(ID3D12Device *, const D3D12_HEAP_PROPERTIES *,
                                                    D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                                                    D3D12_RESOURCE_STATES,
                                                    const D3D12_CLEAR_VALUE *, REFIID, void **);
// slot 44: CreatePlacedResourceX(address, desc, state, clear, iid, resource), a resource on
// memory of the title's (Unreal's XboxOneD3D12Texture.cpp names it). The host cannot place a
// resource at an address: a committed one in GPU memory stands for it.
HRESULT STDMETHODCALLTYPE create_placed_resource_x(ID3D12Device *self, std::uint64_t address,
                                                   const D3D12_RESOURCE_DESC *desc,
                                                   D3D12_RESOURCE_STATES state,
                                                   const D3D12_CLEAR_VALUE *clear, REFIID iid,
                                                   void **resource) {
    (void)address;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    return create_committed_resource(self, &heap, D3D12_HEAP_FLAG_NONE, desc, state, clear, iid,
                                     resource);
}
void report_debug_messages();
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
    }
    const auto result = original<Create>(self, 10)(self, desc ? &host : nullptr, iid, state);
    if (FAILED(result)) {
        std::fprintf(stderr, "[Runtime] CreateGraphicsPipelineState: 0x%08lx\n",
                     static_cast<unsigned long>(result));
        report_debug_messages();
    }
    return result;
}
// slot 74: (data, size, out), still to be named; what it is given goes to the log
void STDMETHODCALLTYPE device_slot_74(ID3D12Device *, const std::uint8_t *data, UINT size,
                                      void **out) {
    if (static std::atomic<int> shown{0}; shown++ < 2) {
        std::string text;
        char hex[4];
        for (UINT i = 0; data && i < std::min(size, 96u); ++i) {
            std::snprintf(hex, sizeof hex, "%02x ", data[i]);
            text += hex;
        }
        std::fprintf(stderr, "[Runtime] device slot 74: %u bytes: %s out %p\n", size, text.c_str(),
                     static_cast<void *>(out));
        std::fflush(stderr);
    }
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
        std::fflush(stderr);
        report_debug_messages();
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
// slot 48: CreateCommandQueue(desc, iid, queue), by its arguments at Unreal's call site
HRESULT STDMETHODCALLTYPE create_command_queue(ID3D12Device *self,
                                               const D3D12_COMMAND_QUEUE_DESC *desc, REFIID iid,
                                               void **queue) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);
    const auto result = original<Create>(self, 8)(self, desc, iid, queue);
    if (SUCCEEDED(result) && queue && *queue)
        console_vtable<Family::queue>(*queue, host_queue_slots, queue_slots, [] {
            auto overrides =
                unknown_queue_methods(std::make_index_sequence<queue_slots - host_queue_slots>{});
            overrides.push_back({23, reinterpret_cast<Slot>(&signal_x)});
            overrides.push_back({10, reinterpret_cast<Slot>(&execute_command_lists)});
            return overrides;
        }());
    if (SUCCEEDED(result) && queue && *queue && desc &&
        desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12CommandQueue *expected = nullptr;
        if (direct_queue.compare_exchange_strong(expected,
                                                 static_cast<ID3D12CommandQueue *>(*queue)))
            direct_queue.load()->AddRef();
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
    // the console's D3D12_RESOURCE_STATE_GENERIC_READ (0x8c3) has no index buffer bit, the
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
    }
    const auto result = original<Create>(self, 27)(self, heap, flags, desc ? &host_desc : nullptr,
                                                   state, clear, iid, resource);
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
void report_debug_messages() {
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
// write pointer at +0x38 and its limit at +0x50, between which it records commands inline
// when there is room. The title gets a proxy of the console's size whose inline space is
// always full, so that every command comes through a method, and the proxy hands those to
// the host's list.
struct ListProxy {
    Slot *vtable;
    std::uint8_t fields[0x200 - sizeof(Slot *)];
    ID3D12GraphicsCommandList *host;
    std::atomic<ULONG> references;
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
// registers and stack). Unreal's call sites put Close, Reset, CopyBufferRegion and
// ResourceBarrier where the desktop has them, and SetDescriptorHeaps 4 later: the console's
// own four come after ExecuteBundle (27), at 28 to 31.
constexpr std::size_t host_list_slots = 60, list_slots = 96, unmapped = ~std::size_t{0};
std::array<std::size_t, list_slots> list_map = [] {
    std::array<std::size_t, list_slots> map{};
    for (std::size_t slot = 0; slot < list_slots; ++slot)
        map[slot] = slot <= 27                                 ? slot
                    : slot < 32 || slot - 4 >= host_list_slots ? unmapped
                                                               : slot - 4;
    // CopyTextureRegion with one more argument, which the host's has not
    map[83] = 16;
    return map;
}();
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
    if (is_any<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12CommandList,
               ID3D12GraphicsCommandList>(iid)) {
        ++static_cast<ListProxy *>(self)->references;
        *object = self;
        return S_OK;
    }
    return host_of(self)->QueryInterface(iid, object);
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
HRESULT STDMETHODCALLTYPE close_list(void *self) {
    const auto result = host_of(self)->Close();
    if (FAILED(result))
        report_debug_messages();
    return result;
}
// ResourceBarrier: transitions name states the console's way (GENERIC_READ is 0x8c3 there)
D3D12_RESOURCE_STATES host_state(D3D12_RESOURCE_STATES state) {
    return state == static_cast<D3D12_RESOURCE_STATES>(0x8c3) ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                              : state;
}
void STDMETHODCALLTYPE resource_barrier(void *self, UINT count,
                                        const D3D12_RESOURCE_BARRIER *barriers) {
    std::vector<D3D12_RESOURCE_BARRIER> host(barriers, barriers + (barriers ? count : 0));
    for (auto &barrier : host)
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            barrier.Transition.StateBefore = host_state(barrier.Transition.StateBefore);
            barrier.Transition.StateAfter = host_state(barrier.Transition.StateAfter);
        }
    host_of(self)->ResourceBarrier(count, host.data());
}
void STDMETHODCALLTYPE execute_bundle(void *self, ID3D12GraphicsCommandList *bundle) {
    host_of(self)->ExecuteBundle(static_cast<List *>(host_list(bundle)));
}
void STDMETHODCALLTYPE om_set_render_targets(void *self, UINT count, const Handle *targets,
                                             BOOL single, const Handle *depth) {
    std::vector<Handle> host(targets ? (single ? 1 : count) : 0);
    for (std::size_t i = 0; i < host.size(); ++i)
        host[i] = host_handle(targets[i]);
    Handle host_depth{};
    if (depth)
        host_depth = host_handle(*depth);
    host_of(self)->OMSetRenderTargets(count, targets ? host.data() : nullptr, single,
                                      depth ? &host_depth : nullptr);
}
void STDMETHODCALLTYPE clear_depth_stencil(void *self, Handle view, D3D12_CLEAR_FLAGS flags,
                                           FLOAT depth, UINT8 stencil, UINT count,
                                           const D3D12_RECT *rects) {
    host_of(self)->ClearDepthStencilView(host_handle(view), flags, depth, stencil, count, rects);
}
void STDMETHODCALLTYPE clear_render_target(void *self, Handle view, const FLOAT *color, UINT count,
                                           const D3D12_RECT *rects) {
    host_of(self)->ClearRenderTargetView(host_handle(view), color, count, rects);
}
void STDMETHODCALLTYPE clear_uav_uint(void *self, D3D12_GPU_DESCRIPTOR_HANDLE gpu, Handle cpu,
                                      ID3D12Resource *resource, const UINT *values, UINT count,
                                      const D3D12_RECT *rects) {
    host_of(self)->ClearUnorderedAccessViewUint(gpu, host_handle(cpu), resource, values, count,
                                                rects);
}
void STDMETHODCALLTYPE clear_uav_float(void *self, D3D12_GPU_DESCRIPTOR_HANDLE gpu, Handle cpu,
                                       ID3D12Resource *resource, const FLOAT *values, UINT count,
                                       const D3D12_RECT *rects) {
    host_of(self)->ClearUnorderedAccessViewFloat(gpu, host_handle(cpu), resource, values, count,
                                                 rects);
}
template <std::size_t... Index>
std::array<Slot, list_slots> list_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, list_slots> vtable{reinterpret_cast<Slot>(&list_method<Index>)...};
    vtable[0] = reinterpret_cast<Slot>(&list_query);
    vtable[1] = reinterpret_cast<Slot>(&list_add_ref);
    vtable[2] = reinterpret_cast<Slot>(&list_release);
    vtable[9] = reinterpret_cast<Slot>(&close_list);
    vtable[26] = reinterpret_cast<Slot>(&resource_barrier);
    vtable[27] = reinterpret_cast<Slot>(&execute_bundle);
    vtable[50] = reinterpret_cast<Slot>(&om_set_render_targets);
    vtable[51] = reinterpret_cast<Slot>(&clear_depth_stencil);
    vtable[52] = reinterpret_cast<Slot>(&clear_render_target);
    vtable[53] = reinterpret_cast<Slot>(&clear_uav_uint);
    vtable[54] = reinterpret_cast<Slot>(&clear_uav_float);
    return vtable;
}
std::array<Slot, list_slots> list_vtable = list_vtable_of(std::make_index_sequence<list_slots>{});
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
// ID3D12CommandQueue::ExecuteCommandLists: the host's lists for the title's proxies
void STDMETHODCALLTYPE execute_command_lists(ID3D12CommandQueue *self, UINT count,
                                             ID3D12CommandList *const *lists) {
    std::vector<ID3D12CommandList *> host(count);
    for (UINT i = 0; i < count; ++i)
        host[i] = host_list(lists[i]);
    original<void(STDMETHODCALLTYPE *)(void *, UINT, ID3D12CommandList *const *)>(self, 10)(
        self, count, host.data());
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
constexpr std::size_t host_device_slots = 44, device_slots = 96;
template <std::size_t... Index>
std::vector<std::pair<std::size_t, Slot>> unknown_device_methods(std::index_sequence<Index...>) {
    return {{host_device_slots + Index,
             reinterpret_cast<Slot>(&unknown_device_method<host_device_slots + Index>)}...};
}

// D3D12XBOX_CREATE_DEVICE_PARAMETERS sizes the console's rings, which mean nothing here
HRESULT WINAPI xbox_create_device(void *, const void *, REFIID iid, void **device) {
    if (!device)
        return E_POINTER;
    *device = nullptr;
    using Create = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateDevice");
    if (std::getenv("XS_D3D_DEBUG")) {
        using GetDebug = HRESULT(WINAPI *)(REFIID, void **);
        static const auto get_debug = host_d3d12<GetDebug>("D3D12GetDebugInterface");
        if (ID3D12Debug *debug = nullptr; get_debug && SUCCEEDED(get_debug(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug->Release();
        }
    }
    ID3D12Device *host = nullptr;
    const auto result =
        create ? create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host)) : E_NOTIMPL;
    if (ID3D12InfoQueue *queue = nullptr;
        SUCCEEDED(result) && SUCCEEDED(host->QueryInterface(IID_PPV_ARGS(&queue))))
        debug_messages = queue;
    std::fprintf(stderr, "[Runtime] Direct3D 12 device on the host: 0x%08lx\n",
                 static_cast<unsigned long>(result));
    std::fflush(stderr);
    if (FAILED(result))
        return result;
    auto overrides =
        unknown_device_methods(std::make_index_sequence<device_slots - host_device_slots>{});
    overrides.push_back({0, reinterpret_cast<Slot>(&device_query)});
    overrides.push_back({13, reinterpret_cast<Slot>(&check_feature_support)});
    overrides.push_back({14, reinterpret_cast<Slot>(&create_descriptor_heap)});
    overrides.push_back({17, reinterpret_cast<Slot>(&create_cbv)});
    overrides.push_back({18, reinterpret_cast<Slot>(&create_srv)});
    overrides.push_back({19, reinterpret_cast<Slot>(&create_uav)});
    overrides.push_back({20, reinterpret_cast<Slot>(&create_target_view<20>)});
    overrides.push_back({21, reinterpret_cast<Slot>(&create_target_view<21>)});
    overrides.push_back({22, reinterpret_cast<Slot>(&create_sampler)});
    overrides.push_back({23, reinterpret_cast<Slot>(&copy_descriptors)});
    overrides.push_back({24, reinterpret_cast<Slot>(&copy_descriptors_simple)});
    overrides.push_back({25, reinterpret_cast<Slot>(&resource_allocation_info)});
    overrides.push_back({26, reinterpret_cast<Slot>(&custom_heap_properties)});
    overrides.push_back({27, reinterpret_cast<Slot>(&create_committed_resource)});
    overrides.push_back({44, reinterpret_cast<Slot>(&create_placed_resource_x)});
    overrides.push_back({8, reinterpret_cast<Slot>(&create_command_queue)});
    overrides.push_back({62, reinterpret_cast<Slot>(&create_compute_pipeline_x)});
    overrides.push_back({74, reinterpret_cast<Slot>(&device_slot_74)});
    overrides.push_back({10, reinterpret_cast<Slot>(&create_graphics_pipeline)});
    overrides.push_back({11, reinterpret_cast<Slot>(&create_compute_pipeline)});
    overrides.push_back({48, reinterpret_cast<Slot>(&create_command_queue)});
    overrides.push_back({73, reinterpret_cast<Slot>(&create_command_list)});
    overrides.push_back({12, reinterpret_cast<Slot>(&create_command_list_desktop)});
    // all of the host's vtable, which its runtime calls past the desktop's first 44 too
    console_vtable<Family::device>(host, device_slots, device_slots, overrides);
    const auto queried = host->QueryInterface(iid, device);
    host->Release();
    return queried;
}
} // namespace

// IDXGIDevice of our device: the host's, with GetAdapter leading to our factory
HRESULT d3d11x::device_dxgi(IUnknown *host_device, REFIID, void **object) {
    IDXGIDevice1 *host = nullptr;
    const auto result = host_device->QueryInterface(&host);
    if (FAILED(result))
        return result;
    *object = make_forwarder(host, 14, &dxgi_device_query,
                             {{6, slot(&device_parent)}, {7, slot(&device_adapter)}});
    return S_OK;
}

void install_d3d11x(Registry &registry) {
    const std::pair<const char *, void *> functions[] = {
        {"D3D11CreateDevice", reinterpret_cast<void *>(&create_device)},
        {"D3D11XCreateDeviceX", reinterpret_cast<void *>(&create_device_x)},
        {"DXGIXPresentArray", reinterpret_cast<void *>(&present_array)},
        {"DXGIXGetFrameStatistics", reinterpret_cast<void *>(&frame_statistics)},
        {"D3DMapEsramMemory", reinterpret_cast<void *>(&map_esram)},
        {"DXGIXSetVLineNotification", reinterpret_cast<void *>(&ignored)},
        {"DXGIXSetFrameNotification", reinterpret_cast<void *>(&ignored)}};
    for (const auto &[name, function] : functions)
        registry.add("d3d11_x.dll", name, function);
    const std::pair<const char *, void *> d3d12[] = {
        {"DXGIXPresentArray", reinterpret_cast<void *>(&present_array)},
        {"DXGIXGetFrameStatistics", reinterpret_cast<void *>(&frame_statistics)},
        {"D3DMapEsramMemory", reinterpret_cast<void *>(&map_esram)},
        {"DXGIXSetVLineNotification", reinterpret_cast<void *>(&ignored)},
        {"DXGIXSetFrameNotification", reinterpret_cast<void *>(&ignored)},
        {"D3D12XboxSetProcessDebugFlags", reinterpret_cast<void *>(&ignored)},
        {"D3D12SerializeRootSignature", reinterpret_cast<void *>(&serialize_root_signature)},
        {"D3D12SerializeVersionedRootSignature",
         reinterpret_cast<void *>(&serialize_versioned_root_signature)},
        {"D3D12CreateDevice", reinterpret_cast<void *>(&create_device_12)},
        {"D3D12XboxCreateDevice", reinterpret_cast<void *>(&xbox_create_device)}};
    for (const auto &[name, function] : d3d12)
        registry.add("d3d12_x.dll", name, function);
}
} // namespace xs::runtime::durango
