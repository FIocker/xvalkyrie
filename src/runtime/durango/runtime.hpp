#pragma once

namespace xs::runtime {
class Registry;
namespace durango {
// Adds the Xbox One console modules (d3d12_x.dll and friends) to the registry.
void install(Registry &registry);
} // namespace durango
} // namespace xs::runtime
