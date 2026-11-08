#pragma once
#include "core/target.hpp"
#include <span>
#include <windows.h>

namespace xs::runtime {
class Registry;
namespace modules {
struct ApiFunction {
    const char *name;
    void *function;
};
// The Win32 functions a console answers differently from the host, by their Win32 names
std::span<const ApiFunction> kernel_functions();
// The same for every API set and system DLL a GDK title imports them from, and what tells
// such a title which console it runs on
void install_kernel(Registry &registry, Target target);
// GetProcAddress for modules of ours and of the title's, then the host's
FARPROC get_proc_address(HMODULE module, const char *name);
} // namespace modules
} // namespace xs::runtime
