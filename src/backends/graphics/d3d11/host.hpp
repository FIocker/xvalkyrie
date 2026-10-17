#pragma once
#include <d3d11_1.h>

// The `d3d11` backend: the host's own Direct3D 11, which a console Direct3D module hands
// the title's calls to one for one. Windows only. Nothing here knows about a console.
namespace xs::graphics::d3d11 {
// Creates the hardware device of the host and its immediate context. With `debug` the
// device is created with the debug layer if the host has it installed, and without it
// otherwise; `debug_layer` tells which one it was. `level` and `context` may be null.
HRESULT create_device(UINT flags, bool debug, const D3D_FEATURE_LEVEL *levels, UINT level_count,
                      ID3D11Device1 **device, D3D_FEATURE_LEVEL *level,
                      ID3D11DeviceContext **context, bool &debug_layer);
// Call right before each Present of a title's swap chain. Counts the frames for the log and
// does what XS_FRAME_DUMP asks for.
void frame_presented(IDXGISwapChain *swap_chain);
} // namespace xs::graphics::d3d11
