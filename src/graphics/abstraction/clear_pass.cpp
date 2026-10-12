#include "graphics/abstraction/clear_pass.hpp"
#include <stdexcept>

namespace xs::graphics {
std::size_t ClearPass::validated_pixel_count() const {
    if (!width || !height || width > 2048 || height > 2048)
        throw std::invalid_argument("Offscreen extent must be 1..2048 texels per dimension");
    if (rectangles.size() > 1024)
        throw std::invalid_argument("Too many offscreen rectangle clears");
    for (const auto &r : rectangles) {
        if (!r.width || !r.height || r.x > width || r.y > height || r.width > width - r.x ||
            r.height > height - r.y)
            throw std::invalid_argument("Clear rectangle is empty or outside the color attachment");
    }
    return static_cast<std::size_t>(width) * height;
}
} // namespace xs::graphics
