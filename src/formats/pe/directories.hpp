#pragma once
#include "formats/pe/image.hpp"
#include <nlohmann/json_fwd.hpp>

namespace xv::pe {
// `entries`: list every exception record and relocation; otherwise only how many there are
// (relocation blocks keep where their data is, which is what applying them needs).
nlohmann::json decode_directories(const Image &image, Bytes descriptors, std::uint64_t base,
                                  std::uint64_t machine, bool entries = true);
} // namespace xv::pe
