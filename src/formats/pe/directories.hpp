#pragma once
#include "formats/pe/image.hpp"
#include <nlohmann/json_fwd.hpp>

namespace xs::pe {
nlohmann::json decode_directories(const Image &image, Bytes descriptors, std::uint64_t base,
                                  std::uint64_t machine);
}
