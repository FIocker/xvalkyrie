#pragma once

namespace xs::runtime {
class Registry;
namespace modules {
// DirectStorage for Series titles: dstorage_xs.dll and XCRDAPI.dll
void install_dstorage(Registry &registry);
} // namespace modules
} // namespace xs::runtime
