#pragma once
#include <cstddef>
#include <cstdint>
#include <d3d11.h>

// Titles of later console OS builds bind resources without calling anything: their headers
// copy descriptors out of the resource objects into packets they append to the context's
// constant-engine buffer (header: register | (dwords - 1) << 16). Ours carry what it takes
// to find the host object again, and the packets are read at the next draw.
namespace xv::runtime::durango::d3d11x {
// fills the descriptor fields of our object for `host`, whatever kind it is
void describe(void *object, IUnknown *host);
// a buffer at the title's memory `address` (a placement buffer)
void describe_buffer(void *object, ID3D11Buffer *host, const void *address);
// how far into `host` the GPU address `address` is, if it is in it
bool buffer_offset(ID3D11Buffer *host, std::uint64_t address, UINT &offset);
// binds what the packets between `start` and `end` named, as the draw about to happen needs
void apply_descriptors(ID3D11DeviceContext *context, const std::byte *start, const std::byte *end);
} // namespace xv::runtime::durango::d3d11x
