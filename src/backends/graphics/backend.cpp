#include "backends/graphics/backend.hpp"
#include <atomic>
#include <stdexcept>
#include <string>

namespace xs::graphics {
namespace {
#ifdef _WIN32
constexpr bool has_d3d11 = true;
#else
constexpr bool has_d3d11 = false;
#endif
std::atomic<Backend> selected{has_d3d11 ? Backend::d3d11 : Backend::vulkan};
} // namespace

std::string_view backend_name(Backend backend) {
    return backend == Backend::d3d11 ? "d3d11" : "vulkan";
}
std::optional<Backend> parse_backend(std::string_view name) {
    for (const auto backend : {Backend::d3d11, Backend::vulkan})
        if (name == backend_name(backend))
            return backend;
    return std::nullopt;
}
bool backend_available(Backend backend) { return backend == Backend::vulkan || has_d3d11; }
bool backend_renders_titles(Backend backend) { return backend == Backend::d3d11 && has_d3d11; }
Backend default_backend() { return has_d3d11 ? Backend::d3d11 : Backend::vulkan; }
Backend selected_backend() { return selected; }
void select_backend(Backend backend) {
    if (!backend_available(backend))
        throw std::invalid_argument("The " + std::string(backend_name(backend)) +
                                    " backend is not available on this host");
    selected = backend;
}
} // namespace xs::graphics
