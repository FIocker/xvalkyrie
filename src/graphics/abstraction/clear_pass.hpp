#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xs::graphics {
using Rgba8 = std::array<std::uint8_t, 4>;

struct RectClear {
    std::uint32_t x, y, width, height;
    Rgba8 color;
};

// Normalized host commands, not a guest ABI. Rectangles are applied in list order.
struct ClearPass {
    std::uint32_t width, height;
    Rgba8 background;
    std::vector<RectClear> rectangles;
    std::size_t validated_pixel_count() const;
};
} // namespace xs::graphics
