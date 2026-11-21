#include "backends/graphics/backend.hpp"
#include "host/files.hpp"
#include <atomic>
#include <filesystem>
#include <system_error>

namespace xv::graphics {
namespace {
std::atomic<Backend> selected{Backend::d3d11};
std::atomic<std::uint64_t> frames{0};

bool installed(std::string_view module) {
    std::error_code error;
    return std::filesystem::is_regular_file(host::executable_directory() / module, error);
}
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
bool backend_renders_titles(Backend backend) {
    return backend == Backend::d3d11 ||
           (installed(vulkan_dxgi_module) && installed(vulkan_d3d11_module));
}
Backend selected_backend() { return selected; }
void select_backend(Backend backend) { selected = backend; }
void frame_counted() { ++frames; }
std::uint64_t frames_presented() { return frames; }
} // namespace xv::graphics
