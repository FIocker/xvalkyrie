#pragma once
#include <optional>
#include <string_view>

namespace xs::graphics {
// What a title's Direct3D calls end up on.
enum class Backend {
    d3d11, // the host's own Direct3D 11. Windows only.
    vulkan // every host. Cannot render titles yet.
};
std::string_view backend_name(Backend backend);
std::optional<Backend> parse_backend(std::string_view name);
// exists on this host
bool backend_available(Backend backend);
// can show what a title draws
bool backend_renders_titles(Backend backend);
// Direct3D 11 where there is one, Vulkan elsewhere
Backend default_backend();
// The backend console runtimes use. Process-wide; starts as default_backend().
Backend selected_backend();
// throws if the backend is not available on this host
void select_backend(Backend backend);
} // namespace xs::graphics
