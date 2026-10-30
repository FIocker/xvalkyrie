#pragma once
#include <d3d11.h>

// Placement resources: the title owns their memory and writes it directly, as the console's
// GPU reads it. Here the host's resource is a copy, created from that memory and brought up
// to date before draws and presents once the title has written it again: the memory is kept
// read-only, and the first write to it marks every resource in it dirty.
namespace xs::runtime::durango::d3d11x {
// `resource` lives at `address` in the layout XG computes here (linear, mips in order)
void track_placement(ID3D11Resource *resource, const void *address);
// the data at `address` for a resource described by `desc`, if that memory can be read
bool placement_data(const D3D11_TEXTURE2D_DESC &desc, const void *address,
                    D3D11_SUBRESOURCE_DATA *data, UINT count);
void upload_placements(ID3D11DeviceContext *context);
} // namespace xs::runtime::durango::d3d11x
