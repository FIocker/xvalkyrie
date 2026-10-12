#include "runtime/runtime.hpp"
#include <stdexcept>

namespace xs::runtime {
bool execution_supported() { return false; }
void *detail::runtime_override(std::string_view, std::string_view) { return nullptr; }
void *detail::host_export(std::string_view, std::string_view) { return nullptr; }
std::uint32_t execute(const std::filesystem::path &, std::span<const std::uint8_t>,
                      const Registry &, bool) {
    throw std::runtime_error("Guest execution requires a Windows x64 host");
}
} // namespace xs::runtime
