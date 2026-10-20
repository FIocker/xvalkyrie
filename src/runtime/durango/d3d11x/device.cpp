#include "backends/graphics/backend.hpp"
#include "backends/graphics/d3d11/host.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/common/windows/com_forward.hpp"
#include "runtime/durango/d3d11x/wrap.hpp"
#include "runtime/durango/runtime.hpp"
#include "runtime/durango/winrt/application.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <d3d11_1.h>
#include <dxgi1_2.h>

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
HRESULT factory_query(Forwarder *self, REFIID iid, void **object) {
    if (is_any<IUnknown, IDXGIObject, IDXGIFactory, IDXGIFactory1, IDXGIFactory2>(iid))
        return keep(self, object);
    return forwarder_host(self)->QueryInterface(iid, object);
}

// IDXGIObject::GetParent of the adapter
HRESULT STDMETHODCALLTYPE adapter_parent(Forwarder *self, REFIID iid, void **parent) {
    auto *const adapter = static_cast<IDXGIAdapter *>(forwarder_host(self));
    if (!is_any<IDXGIFactory, IDXGIFactory1, IDXGIFactory2>(iid))
        return adapter->GetParent(iid, parent);
    IDXGIFactory2 *factory = nullptr;
    const auto result =
        adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory));
    if (FAILED(result))
        return result;
    *parent = make_forwarder(factory, 25, &factory_query, {{16, slot(&create_swap_chain)}});
    return S_OK;
}
HRESULT adapter_query(Forwarder *self, REFIID iid, void **object) {
    if (is_any<IUnknown, IDXGIObject, IDXGIAdapter, IDXGIAdapter1, IDXGIAdapter2>(iid))
        return keep(self, object);
    return forwarder_host(self)->QueryInterface(iid, object);
}

// IDXGIDevice::GetAdapter
HRESULT STDMETHODCALLTYPE device_adapter(Forwarder *self, IDXGIAdapter **adapter) {
    IDXGIAdapter *host = nullptr;
    auto result = static_cast<IDXGIDevice *>(forwarder_host(self))->GetAdapter(&host);
    if (FAILED(result))
        return result;
    IDXGIAdapter2 *latest = nullptr;
    result = host->QueryInterface(&latest);
    host->Release();
    if (FAILED(result))
        return result;
    *adapter = reinterpret_cast<IDXGIAdapter *>(
        make_forwarder(latest, 12, &adapter_query, {{6, slot(&adapter_parent)}}));
    return S_OK;
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
        std::fprintf(stderr, "[Runtime] the %s backend cannot render titles yet\n",
                     std::string(graphics::backend_name(backend)).c_str());
        std::fflush(stderr);
        return DXGI_ERROR_UNSUPPORTED;
    }
    // Console-only flag bits and the debug layer, which may not be installed, are dropped.
    const UINT host_flags =
        flags & (D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                 D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS);
    ID3D11Device1 *latest = nullptr;
    bool debug_layer = false;
    const auto result =
        graphics::d3d11::create_device(host_flags, std::getenv("XS_D3D_DEBUG") != nullptr, levels,
                                       level_count, &latest, level, context, debug_layer);
    if (debug_layer)
        d3d11x::watch_debug_messages(latest);
    std::fprintf(stderr, "[Runtime] Direct3D 11 device on the host (flags 0x%x): 0x%08lx\n", flags,
                 static_cast<unsigned long>(result));
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
} // namespace

// IDXGIDevice of our device: the host's, with GetAdapter leading to our factory
HRESULT d3d11x::device_dxgi(IUnknown *host_device, REFIID, void **object) {
    IDXGIDevice1 *host = nullptr;
    const auto result = host_device->QueryInterface(&host);
    if (FAILED(result))
        return result;
    *object = make_forwarder(host, 14, &dxgi_device_query, {{7, slot(&device_adapter)}});
    return S_OK;
}

void install_d3d11x(Registry &registry) {
    registry.add("d3d11_x.dll", "D3D11CreateDevice", reinterpret_cast<void *>(&create_device));
}
} // namespace xs::runtime::durango
