#pragma once
#include "core/target.hpp"
#include "runtime/common/runtime.hpp"
#include <cstdint>
#include <span>

namespace xs::runtime {
// Our modules for one console: what both share plus its own runtime.
Registry console_registry(Target target);
// Registry for the console the image's imports point to, empty if that is unknown.
Registry title_registry(std::span<const std::uint8_t> file);
} // namespace xs::runtime
