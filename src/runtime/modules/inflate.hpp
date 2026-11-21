#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace xv::runtime::modules {
// Inflates a zlib stream (RFC 1950) onto the end of `out`; false if it is not one
bool inflate_zlib(std::span<const std::uint8_t> stream, std::vector<std::uint8_t> &out);
} // namespace xv::runtime::modules
