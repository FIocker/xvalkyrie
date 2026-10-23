#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace xs::graphics {
// What a title's Direct3D calls end up on.
enum class Backend {
    d3d11, // the host's own Direct3D 11
    vulkan // Direct3D 11 translated to Vulkan by xs_d3d11.dll and xs_dxgi.dll
};
inline constexpr std::string_view vulkan_dxgi_module = "xs_dxgi.dll";
inline constexpr std::string_view vulkan_d3d11_module = "xs_d3d11.dll";
std::string_view backend_name(Backend backend);
std::optional<Backend> parse_backend(std::string_view name);
// can show what a title draws
bool backend_renders_titles(Backend backend);
// The backend console runtimes use. Process-wide; starts as d3d11.
Backend selected_backend();
void select_backend(Backend backend);

// How many frames titles have presented in this process, whatever the backend. A backend
// calls frame_counted() once per Present; the difference between two readings over the
// time between them is the frame rate.
void frame_counted();
std::uint64_t frames_presented();
} // namespace xs::graphics
