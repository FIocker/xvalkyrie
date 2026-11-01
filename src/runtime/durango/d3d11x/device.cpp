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
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_6.h>
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
HRESULT STDMETHODCALLTYPE create_swap_chain(Forwarder *self, IUnknown *device, IUnknown *,
                                            const DXGI_SWAP_CHAIN_DESC1 *description, IDXGIOutput *,
                                            IDXGISwapChain1 **swap_chain) {
    auto *const window = winrt::current_title_window();
    if (!window || !description || !swap_chain)
        return E_INVALIDARG;
    auto *const factory = static_cast<IDXGIFactory2 *>(forwarder_host(self));
    // what a CoreWindow takes and a window handle does not
    auto host = *description;
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
    const auto result = factory->CreateSwapChainForHwnd(
        static_cast<IUnknown *>(d3d11x::unwrap(device)), static_cast<HWND>(window->native()), &host,
        nullptr, nullptr, swap_chain);
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
void console_vtable(void *object, std::size_t host_slots, std::size_t slots,
                    const std::vector<std::pair<std::size_t, Slot>> &overrides) {
    auto *const host = *static_cast<Slot **>(object);
    auto *const block = new Slot[slots + 1]{};
    block[0] = host;
    std::copy(host, host + host_slots, block + 1);
    for (const auto &[index, function] : overrides)
        block[1 + index] = function;
    *static_cast<Slot **>(object) = block + 1;
}
// Methods that return an 8-byte structure: the console's return it in RAX, where the
// desktop's C++ methods write it through a hidden pointer
template <std::size_t Index> std::uint64_t STDMETHODCALLTYPE returned_in_rax(void *self) {
    std::uint64_t value = 0;
    original<void *(STDMETHODCALLTYPE *)(void *, std::uint64_t *)>(self, Index)(self, &value);
    return value;
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
// CreateDescriptorHeap: the heap's Get{CPU,GPU}DescriptorHandleForHeapStart (9, 10) return
// their handles in RAX
HRESULT STDMETHODCALLTYPE create_descriptor_heap(ID3D12Device *self,
                                                 const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID iid,
                                                 void **heap) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **);
    const auto result = original<Create>(self, 14)(self, desc, iid, heap);
    if (SUCCEEDED(result) && heap && *heap)
        console_vtable(*heap, 11, 11,
                       {{9, reinterpret_cast<Slot>(&returned_in_rax<9>)},
                        {10, reinterpret_cast<Slot>(&returned_in_rax<10>)}});
    return result;
}
// slot 48: CreateCommandQueue(desc, iid, queue), by its arguments at Unreal's call site
HRESULT STDMETHODCALLTYPE create_command_queue(ID3D12Device *self,
                                               const D3D12_COMMAND_QUEUE_DESC *desc, REFIID iid,
                                               void **queue) {
    return self->CreateCommandQueue(desc, iid, queue);
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
    const auto result =
        original<Create>(self, 27)(self, heap, flags, desc, state, clear, iid, resource);
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
// slot 73: a command list from a description (Unreal's call site: the type, 0, the node mask,
// 0), an allocator, a pipeline state, then the interface and the list
HRESULT STDMETHODCALLTYPE create_command_list(ID3D12Device *self, const UINT *desc,
                                              ID3D12CommandAllocator *allocator,
                                              ID3D12PipelineState *state, REFIID iid, void **list) {
    if (!desc)
        return E_INVALIDARG;
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] console CreateCommandList description %u %u %u %u\n",
                     desc[0], desc[1], desc[2], desc[3]);
        std::fflush(stderr);
    }
    return self->CreateCommandList(desc[2], static_cast<D3D12_COMMAND_LIST_TYPE>(desc[0]),
                                   allocator, state, iid, list);
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
    ID3D12Device *host = nullptr;
    const auto result =
        create ? create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host)) : E_NOTIMPL;
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
    overrides.push_back({27, reinterpret_cast<Slot>(&create_committed_resource)});
    overrides.push_back({48, reinterpret_cast<Slot>(&create_command_queue)});
    overrides.push_back({73, reinterpret_cast<Slot>(&create_command_list)});
    console_vtable(host, host_device_slots, device_slots, overrides);
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
