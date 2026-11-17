#pragma once
#include "core/target.hpp"
#include <span>
#include <string>
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
// The host's path for one the title names: its package and its storage for the console's
// drives, as the file functions here have it
std::wstring console_file_path(const wchar_t *path);
// GetProcAddress for modules of ours and of the title's, then the host's
FARPROC get_proc_address(HMODULE module, const char *name);
} // namespace modules
} // namespace xs::runtime
