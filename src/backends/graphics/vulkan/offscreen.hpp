#pragma once
#include "backends/graphics/clear_pass.hpp"
#include <string>

namespace xv::graphics::vulkan {
struct Readback {
    std::string adapter;
    std::vector<Rgba8> pixels;
};
Readback render(const ClearPass &pass);
} // namespace xv::graphics::vulkan
