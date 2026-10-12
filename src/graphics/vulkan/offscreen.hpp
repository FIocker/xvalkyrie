#pragma once
#include "graphics/abstraction/clear_pass.hpp"
#include <string>

namespace xs::graphics::vulkan {
struct Readback {
    std::string adapter;
    std::vector<Rgba8> pixels;
};
Readback render(const ClearPass &pass);
} // namespace xs::graphics::vulkan
