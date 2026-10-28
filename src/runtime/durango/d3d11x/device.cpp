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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <utility>

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

// IDXGIDevice::GetAdapter
HRESULT STDMETHODCALLTYPE device_adapter(Forwarder *self, IDXGIAdapter **adapter) {
    IDXGIAdapter *host = nullptr;
    auto result = static_cast<IDXGIDevice *>(forwarder_host(self))->GetAdapter(&host);
    if (FAILED(result))
        return result;
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
}
} // namespace xs::runtime::durango
