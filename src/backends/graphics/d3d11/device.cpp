#include "backends/graphics/d3d11/host.hpp"
#include <cstdio>

namespace xv::graphics::d3d11 {
HRESULT create_device(UINT flags, bool debug, const D3D_FEATURE_LEVEL *levels, UINT level_count,
                      ID3D11Device1 **device, D3D_FEATURE_LEVEL *level,
                      ID3D11DeviceContext **context, bool &debug_layer) {
    *device = nullptr;
    debug_layer = false;
    ID3D11Device *host = nullptr;
    auto result = E_FAIL;
    if (debug) {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags | D3D11_CREATE_DEVICE_DEBUG, levels, level_count,
                                   D3D11_SDK_VERSION, &host, level, context);
        debug_layer = SUCCEEDED(result);
        if (!debug_layer)
            std::fprintf(stderr, "[Runtime] no Direct3D debug layer on this host (0x%08lx)\n",
                         static_cast<unsigned long>(result));
    }
    if (FAILED(result))
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   level_count, D3D11_SDK_VERSION, &host, level, context);
    if (SUCCEEDED(result)) {
        result = host->QueryInterface(device);
        host->Release();
    }
    if (FAILED(result))
        debug_layer = false;
    return result;
}
} // namespace xv::graphics::d3d11
