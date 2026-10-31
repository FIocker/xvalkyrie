#pragma once
#include <cstddef>
#include <cstdint>
#include <d3d11.h>

// Placement resources: the title owns their memory and writes it directly, as the console's
// GPU reads it. Here the host's resource is a copy, created from that memory and brought up
// to date before draws and presents once the title has written it again: the memory is kept
// read-only, and the first write to it marks every resource in it dirty.
namespace xs::runtime::durango::d3d11x {
// `resource` lives at `address`: linear (tile mode 0 or 1, the layout our XG computes) or in
// a tiling the console's xg_x.dll copies out of, when the title's system folder has it
void track_placement(ID3D11Resource *resource, const void *address, UINT tile_mode = 0,
                     UINT pitch = 0);
// the data at `address` for a resource described by `desc`, if that memory can be read
bool placement_data(const D3D11_TEXTURE2D_DESC &desc, const void *address,
                    D3D11_SUBRESOURCE_DATA *data, UINT count);
void upload_placements(ID3D11DeviceContext *context);
// every page of [address, address + bytes) is committed and readable
bool readable_memory(std::uint64_t address, std::size_t bytes);
} // namespace xs::runtime::durango::d3d11x
