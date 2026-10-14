#include "runtime/durango/xg.hpp"
#include "runtime/common/runtime.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace xs::runtime::durango {
FormatSize format_size(std::uint32_t format) {
    const auto within = [&](std::uint32_t first, std::uint32_t last) {
        return format >= first && format <= last;
    };
    if (within(1, 4)) // R32G32B32A32
        return {16, false};
    if (within(5, 8)) // R32G32B32
        return {12, false};
    if (within(9, 22)) // R16G16B16A16, R32G32, D32 + S8
        return {8, false};
    if (within(23, 47)) // 10:10:10:2, 11:11:10, R8G8B8A8, R16G16, R32, D24 + S8
        return {4, false};
    if (within(48, 59)) // R8G8, R16
        return {2, false};
    if (within(60, 65)) // R8, A8
        return {1, false};
    if (within(70, 72) || within(79, 81)) // BC1, BC4
        return {8, true};
    if (within(73, 78) || within(82, 84) || within(94, 99)) // BC2, BC3, BC5, BC6H, BC7
        return {16, true};
    if (format == 85 || format == 86 || format == 115) // 5:6:5, 5:5:5:1, 4:4:4:4
        return {2, false};
    return {4, false}; // B8G8R8A8 and friends, and anything unlisted
}

std::uint64_t texture_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                            std::uint32_t array_size, std::uint32_t format) {
    const auto size = format_size(format);
    if (!mips) { // a full chain
        for (auto side = std::max(width, height); side; side >>= 1)
            ++mips;
    }
    std::uint64_t total = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const std::uint64_t w = std::max(width >> mip, 1u), h = std::max(height >> mip, 1u);
        total += size.blocks ? ((w + 3) / 4) * ((h + 3) / 4) * size.bytes : w * h * size.bytes;
    }
    total *= std::max(array_size, 1u);
    return (total + 4095) & ~std::uint64_t{4095};
}

namespace {
// XG_TEXTURE2D_DESC as titles fill it in: the Direct3D 11.X texture description followed
// by the tile mode and the pitch.
struct TextureDescription {
    std::uint32_t width, height, mips, array_size, format, sample_count, sample_quality, usage,
        bind_flags, cpu_access, misc_flags, esram_offset, esram_usage, tile_mode, pitch;
};
// Titles read the total size from the start of XG_RESOURCE_LAYOUT. The structure goes on
// with per-plane and per-mip tables, 0xd40 bytes in all, which are left zeroed.
constexpr std::size_t layout_bytes = 0xd40;
struct Computer {
    void *const *vtable;
    std::atomic<std::uint32_t> references;
    TextureDescription description;
};
std::uint32_t XS_GUEST_ABI add_ref(Computer *self) { return ++self->references; }
std::uint32_t XS_GUEST_ABI release(Computer *self) {
    const auto left = --self->references;
    if (!left)
        delete self;
    return left;
}
std::int32_t XS_GUEST_ABI resource_layout(Computer *self, void *layout) {
    std::memset(layout, 0, layout_bytes);
    const auto &d = self->description;
    const std::uint64_t header[2] = {
        texture_bytes(d.width, d.height, d.mips, d.array_size, d.format), 4096};
    std::memcpy(layout, header, sizeof header);
    const std::uint32_t counts[2] = {d.mips, 1}; // mip levels, planes
    std::memcpy(static_cast<char *>(layout) + sizeof header, counts, sizeof counts);
    return 0;
}
template <int Slot> std::int32_t XS_GUEST_ABI unknown(Computer *) {
    std::fprintf(stderr, "[Runtime] unimplemented xg_x.dll texture computer method %d\n", Slot);
    std::fflush(stderr);
    return static_cast<std::int32_t>(0x80004001); // E_NOTIMPL
}
void *const computer_vtable[] = {
    reinterpret_cast<void *>(&add_ref),         reinterpret_cast<void *>(&release),
    reinterpret_cast<void *>(&resource_layout), reinterpret_cast<void *>(&unknown<3>),
    reinterpret_cast<void *>(&unknown<4>),      reinterpret_cast<void *>(&unknown<5>),
    reinterpret_cast<void *>(&unknown<6>),      reinterpret_cast<void *>(&unknown<7>),
    reinterpret_cast<void *>(&unknown<8>),      reinterpret_cast<void *>(&unknown<9>),
    reinterpret_cast<void *>(&unknown<10>),     reinterpret_cast<void *>(&unknown<11>),
};

std::int32_t XS_GUEST_ABI create_texture2d_computer(const TextureDescription *description,
                                                    Computer **computer) {
    if (!description || !computer)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    *computer = new Computer{computer_vtable, 1, *description};
    return 0;
}
// There is one layout, so one tile mode: 0.
std::uint32_t XS_GUEST_ABI optimal_tile_mode(std::uint32_t, std::uint32_t, std::uint32_t,
                                             std::uint32_t, std::uint32_t, std::uint32_t,
                                             std::uint32_t) {
    return 0;
}
std::int32_t XS_GUEST_ABI optimal_depth_stencil_tile_modes(std::uint32_t, std::uint32_t,
                                                           std::uint32_t, std::uint32_t,
                                                           std::uint32_t, std::uint32_t,
                                                           std::uint32_t *depth,
                                                           std::uint32_t *stencil) {
    if (depth)
        *depth = 0;
    if (stencil)
        *stencil = 0;
    return 0;
}
} // namespace

void install_xg(Registry &registry) {
    registry.add("xg_x.dll", "XGCreateTexture2DComputer",
                 reinterpret_cast<void *>(&create_texture2d_computer));
    registry.add("xg_x.dll", "XGComputeOptimalTileMode",
                 reinterpret_cast<void *>(&optimal_tile_mode));
    registry.add("xg_x.dll", "XGComputeOptimalDepthStencilTileModes",
                 reinterpret_cast<void *>(&optimal_depth_stencil_tile_modes));
}
} // namespace xs::runtime::durango
