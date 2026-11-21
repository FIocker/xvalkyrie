#pragma once
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <span>

namespace xv::pe {
// How much of the large tables the report spells out. Both check every table the same way.
enum class Detail {
    full,  // every exception record and every base relocation
    light, // their counts only: what loading and running need, at a fraction of the cost
};
nlohmann::json inspect(std::span<const std::uint8_t> bytes, Detail detail = Detail::full);
} // namespace xv::pe
