#pragma once
#include <d3d11_1.h>

namespace xv::graphics::dxvk {
HRESULT create_device(UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count,
                      ID3D11Device1 **device, D3D_FEATURE_LEVEL *level,
                      ID3D11DeviceContext **context);
} // namespace xv::graphics::dxvk
