#pragma once
#include <cstddef>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <span>

namespace xv::gdk {
inline constexpr std::size_t config_limit = 1024 * 1024;
nlohmann::json inspect_config(std::span<const std::uint8_t> bytes);
} // namespace xv::gdk
