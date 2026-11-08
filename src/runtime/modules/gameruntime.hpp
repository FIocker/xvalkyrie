#pragma once

namespace xs::runtime {
class Registry;
namespace modules {
// xgameruntime.dll: the system side of the GDK's Gaming Runtime, which a title's own half
// loads by name when it starts. One signed-in user, saves in files, nothing online.
void install_gameruntime(Registry &registry);
} // namespace modules
} // namespace xs::runtime
