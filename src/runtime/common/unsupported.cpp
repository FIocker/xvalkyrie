#include "runtime/common/runtime.hpp"
#include <stdexcept>

namespace xs::runtime {
bool execution_supported() { return false; }
std::uint64_t detail::guest_offset(const void *) { return 0; }
void *detail::runtime_override(std::string_view) { return nullptr; }
void *detail::host_export(std::string_view, std::string_view) { return nullptr; }
std::uint32_t execute(const std::filesystem::path &, std::span<const std::uint8_t>,
                      const Registry &, const ExecuteOptions &) {
    throw std::runtime_error("Guest execution requires a Windows x64 host");
}
} // namespace xs::runtime
