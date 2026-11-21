#include "backends/graphics/vulkan/offscreen.hpp"
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

int main() {
    try {
        using namespace xv::graphics;
        const Rgba8 background{13, 47, 73, 255}, first{191, 23, 87, 128}, last{37, 211, 83, 0};
        const ClearPass pass{37, 23, background, {{0, 3, 21, 20, first}, {10, 0, 27, 11, last}}};
        const auto result = vulkan::render(pass);
        for (std::uint32_t y = 0; y < pass.height; ++y) {
            for (std::uint32_t x = 0; x < pass.width; ++x) {
                const auto expected = y < 11 && x >= 10  ? last
                                      : y >= 3 && x < 21 ? first
                                                         : background;
                if (result.pixels.at(y * pass.width + x) != expected)
                    throw std::runtime_error("RGBA8 readback mismatch at (" + std::to_string(x) +
                                             ", " + std::to_string(y) + ")");
            }
        }
        const nlohmann::json report{{"backend", "host_vulkan"},
                                    {"adapter", result.adapter},
                                    {"operation", "offscreen_rgba8_clear"},
                                    {"width", pass.width},
                                    {"height", pass.height},
                                    {"pixels_verified", result.pixels.size()},
                                    {"guest_commands_executed", false}};
        std::cout << report.dump(2) << '\n';
    } catch (const std::exception &error) {
        std::cerr << "Vulkan host check: " << error.what() << '\n';
        return 1;
    }
}
