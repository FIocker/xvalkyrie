#pragma once
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <span>

namespace xs::xvd {
inline constexpr std::size_t header_size = 0x1000;
bool has_magic(std::span<const std::uint8_t> prefix);
// Parses the header only. No signature checks, no decryption.
nlohmann::json identify(std::span<const std::uint8_t> prefix, std::uint64_t file_size);
} // namespace xs::xvd
