#include "backends/graphics/backend.hpp"
#include "backends/graphics/dxvk/host.hpp"
#include "host/files.hpp"
#include "host/module.hpp"
#include <cstdio>
#include <dxgi1_2.h>
#include <exception>
#include <wrl/client.h>

namespace xs::graphics::dxvk {
namespace {
using Microsoft::WRL::ComPtr;
using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);

struct Modules {
    host::NativeModule dxgi, d3d11;
};
const Modules &modules() {
    static const Modules loaded{
        host::NativeModule(host::executable_directory() / vulkan_dxgi_module),
        host::NativeModule(host::executable_directory() / vulkan_d3d11_module)};
    return loaded;
}
} // namespace

HRESULT create_device(UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count,
                      ID3D11Device1 **device, D3D_FEATURE_LEVEL *level,
                      ID3D11DeviceContext **context) {
    *device = nullptr;
    CreateFactory create_factory = nullptr;
    PFN_D3D11_CREATE_DEVICE create = nullptr;
    try {
        const auto &loaded = modules();
        create_factory =
            reinterpret_cast<CreateFactory>(loaded.dxgi.function("CreateDXGIFactory1"));
        create =
            reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(loaded.d3d11.function("D3D11CreateDevice"));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[Runtime] Direct3D 11 on Vulkan cannot be loaded: %s\n", e.what());
        return DXGI_ERROR_UNSUPPORTED;
    }
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> host;
    auto result = create_factory(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result))
        result = factory->EnumAdapters1(0, &adapter);
    if (SUCCEEDED(result)) {
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        std::fprintf(stderr, "[Runtime] Vulkan adapter: %ls\n", description.Description);
        result = create(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, level_count,
                        D3D11_SDK_VERSION, &host, level, context);
    }
    if (SUCCEEDED(result))
        result = host->QueryInterface(device);
    if (FAILED(result) && context && *context) {
        (*context)->Release();
        *context = nullptr;
    }
    return result;
}
} // namespace xs::graphics::dxvk
