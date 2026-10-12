#pragma once
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <span>

namespace xs::pe {
nlohmann::json inspect(std::span<const std::uint8_t> bytes);
}
