#pragma once
#include "host/memory.hpp"
#include <cstdint>
#include <filesystem>
#include <stdexcept>

namespace xs::host::detail {
std::byte *reserve(std::size_t size);
void release(std::byte *address, std::size_t size) noexcept;
void commit(std::byte *address, std::size_t size);
void protect(std::byte *address, std::size_t size, Access access);

inline void check_path(const std::filesystem::path &path) {
    if (path.empty() || path.native().find('\0') != std::filesystem::path::string_type::npos)
        throw std::invalid_argument("Expected nonempty path without embedded NUL");
}
inline void check_module_path(const std::filesystem::path &path) {
    check_path(path);
    if (!path.is_absolute())
        throw std::invalid_argument("Trusted host module path must be absolute");
}
inline std::size_t checked_size(std::uintmax_t bytes, std::size_t limit) {
    if (bytes > limit)
        throw std::length_error("File exceeds configured byte limit");
    return static_cast<std::size_t>(bytes);
}
} // namespace xs::host::detail
