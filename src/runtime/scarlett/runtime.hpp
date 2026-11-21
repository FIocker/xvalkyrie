#pragma once

namespace xv::runtime {
class Registry;
namespace scarlett {
// Adds the Xbox Series X|S console modules (d3d12_xs.dll and friends) to the registry.
void install(Registry &registry);
} // namespace scarlett
} // namespace xv::runtime
