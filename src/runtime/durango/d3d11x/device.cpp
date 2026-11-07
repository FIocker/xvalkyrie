#include "backends/graphics/backend.hpp"
#include "backends/graphics/d3d11/host.hpp"
#include "backends/graphics/dxvk/host.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/common/com_forward.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/d3d11x/layout.hpp"
#include "runtime/durango/d3d11x/placement.hpp"
#include "runtime/durango/d3d11x/wrap.hpp"
#include "runtime/durango/runtime.hpp"
#include "runtime/durango/winrt/application.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
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
enum class Family { device, list, heap, queue, resource };
constexpr std::size_t max_slots = 128;
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
                                             std::uint64_t c, std::uint64_t d) {
    return reinterpret_cast<std::uint64_t(STDMETHODCALLTYPE *)(void *, std::uint64_t, std::uint64_t,
                                                               std::uint64_t, std::uint64_t)>(
        original_vtable(self)[Host])(self, a, b, c, d);
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
    bool shader_visible;
    std::uint64_t gpu; // the host's GPU handle of the start, for a shader-visible heap
    UINT host_stride;
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
                          device->GetDescriptorHandleIncrementSize(desc.Type)};
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
// CreateRootSignature (16): its descriptor tables are kept, to know what the descriptors the
// title writes are
HRESULT STDMETHODCALLTYPE create_root_signature(ID3D12Device *self, UINT node, const void *blob,
                                                SIZE_T size, REFIID iid, void **signature) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, UINT, const void *, SIZE_T, REFIID, void **);
    const auto result = original<Create>(self, 16)(self, node, blob, size, iid, signature);
    if (SUCCEEDED(result) && signature && *signature)
        read_root_signature(*signature, blob, size);
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
        d3d11x::track_placement(static_cast<ID3D12Resource *>(*resource),
                                reinterpret_cast<const void *>(address));
    return result;
}
void report_d3d12_messages();
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
        report_d3d12_messages();
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
        std::fflush(stderr);
        report_d3d12_messages();
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
        if (direct_queue.compare_exchange_strong(expected, host))
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
void report_d3d12_messages() {
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
    ID3D12Resource *target = nullptr; // bound last, for XS_D3D_PROBE's draw counts
    const void *graphics_root = nullptr, *compute_root = nullptr;
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
constexpr std::size_t host_list_slots = 60, list_slots = 104, unmapped = ~std::size_t{0};
std::array<std::size_t, list_slots> list_map = [] {
    std::array<std::size_t, list_slots> map{};
    for (std::size_t slot = 0; slot < list_slots; ++slot)
        map[slot] = slot <= 27               ? slot
                    : slot < 32              ? slot + 24
                    : slot < 56              ? slot - 4
                    : slot < host_list_slots ? slot
                                             : unmapped;
    // CopyTextureRegion with one more argument, which the host's has not
    map[83] = 16;
    // DispatchX and CopyResourceX likewise: Dispatch and CopyResource, and flags
    map[78] = 14;
    map[84] = 17;
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
std::atomic<ID3D12Device *> d3d12_device{nullptr};
HRESULT STDMETHODCALLTYPE close_list(void *self) {
    const auto result = host_of(self)->Close();
    if (FAILED(result))
        report_d3d12_messages();
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
    ID3D12DescriptorHeap *host[2]{};
    for (UINT i = 0; i < count && i < 2; ++i)
        host[i] = host_heap(heaps[i]);
    host_of(self)->SetDescriptorHeaps(std::min(count, 2u), host);
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
    host_of(self)->SetComputeRootDescriptorTable(parameter, host_gpu(table));
}
void STDMETHODCALLTYPE set_graphics_root_table(void *self, UINT parameter,
                                               D3D12_GPU_DESCRIPTOR_HANDLE table) {
    decode_table(static_cast<ListProxy *>(self)->graphics_root, parameter, table.ptr);
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
void STDMETHODCALLTYPE draw_instanced(void *self, UINT vertices, UINT instances, UINT first,
                                      UINT first_instance) {
    count_draw(self);
    host_of(self)->DrawInstanced(vertices, instances, first, first_instance);
}
void STDMETHODCALLTYPE draw_indexed_instanced(void *self, UINT indices, UINT instances, UINT first,
                                              INT base, UINT first_instance) {
    count_draw(self);
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
    host_of(self)->ClearUnorderedAccessViewUint(host_gpu(gpu), host_view(cpu), resource, values,
                                                count, rects);
}
void STDMETHODCALLTYPE clear_uav_float(void *self, D3D12_GPU_DESCRIPTOR_HANDLE gpu, Handle cpu,
                                       ID3D12Resource *resource, const FLOAT *values, UINT count,
                                       const D3D12_RECT *rects) {
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
    vtable[21] = reinterpret_cast<Slot>(&set_viewports);
    vtable[22] = reinterpret_cast<Slot>(&set_scissors);
    vtable[26] = reinterpret_cast<Slot>(&resource_barrier);
    vtable[27] = reinterpret_cast<Slot>(&execute_bundle);
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
    // stencil controls, NopX, KickoffX, shader limits, compression state and prefetches
    for (const std::size_t hint : {64, 65, 66, 73, 79, 82, 93, 95, 96, 97, 98, 99})
        vtable[hint] = reinterpret_cast<Slot>(&ignored_list_method);
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
    auto uploads = d3d11x::take_d3d12_uploads();
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
    // all of the host's vtable, which its runtime calls past the desktop's first 44 too
    console_vtable<Family::device>(host, device_slots, device_slots, overrides);
    const auto queried = host->QueryInterface(iid, device);
    host->Release();
    return queried;
}
} // namespace

void d3d11x::console_d3d12_resource(void *resource) { console_resource(resource); }
void d3d11x::report_d3d12_failure() {
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
    report_d3d12_messages();
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
ID3D12Resource *d3d11x::last_render_target() { return last_target.load(); }
std::uint64_t d3d11x::draws_into_target(ID3D12Resource *target) {
    const std::lock_guard lock(draw_mutex);
    const auto found = draws_into.find(target);
    return found == draws_into.end() ? 0 : found->second;
}
std::vector<ID3D12Resource *> d3d11x::bound_render_targets() {
    const std::lock_guard lock(target_mutex);
    return bound_targets;
}
bool d3d11x::read_back_d3d12(ID3D12Resource *resource, std::vector<std::uint8_t> &data,
                             unsigned &pitch, unsigned &width, unsigned &height,
                             DXGI_FORMAT &format, D3D12_RESOURCE_STATES state) {
    auto *const queue = direct_queue.load();
    ID3D12Device *device = nullptr;
    if (!queue || FAILED(resource->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    const auto desc = resource->GetDesc();
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
        {"D3D12XboxGetProcessDebugFlags", reinterpret_cast<void *>(&ignored)},
        {"D3DFlushEntireCpuCache", reinterpret_cast<void *>(&ignored)},
        {"D3D12CreateRootSignatureDeserializer",
         reinterpret_cast<void *>(&root_signature_deserializer)},
        {"D3D12CreateVersionedRootSignatureDeserializer",
         reinterpret_cast<void *>(&versioned_root_signature_deserializer)},
        {"D3D12SerializeRootSignature", reinterpret_cast<void *>(&serialize_root_signature)},
        {"D3D12SerializeVersionedRootSignature",
         reinterpret_cast<void *>(&serialize_versioned_root_signature)},
        {"D3D12CreateDevice", reinterpret_cast<void *>(&create_device_12)},
        {"D3D12XboxCreateDevice", reinterpret_cast<void *>(&xbox_create_device)}};
    for (const auto &[name, function] : d3d12)
        registry.add("d3d12_x.dll", name, function);
}
} // namespace xs::runtime::durango
