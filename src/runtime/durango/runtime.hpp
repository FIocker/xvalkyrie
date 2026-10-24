#pragma once

namespace xs::runtime {
class Registry;
namespace durango {
// Adds the Xbox One console modules (d3d12_x.dll and friends) to the registry.
void install(Registry &registry);
// kernelx.dll functions that are not Win32 ones.
void install_kernelx(Registry &registry);
// Console memory: VirtualAlloc with page sizes, physical pages, XMem, graphics and audio
// processor memory (memory.cpp). Part of install_kernelx.
void install_memory(Registry &registry);
// System DLLs of the console with a few console-only functions on top of the desktop ones:
// mmdevapi, mfplat, pixevt, toolhelpx (system.cpp).
void install_system(Registry &registry);
// WinRT activation for C++/CX titles.
void install_winrt(Registry &registry);
// d3d11_x.dll. Only on hosts with a Direct3D 11 of their own.
void install_d3d11x(Registry &registry);
} // namespace durango
} // namespace xs::runtime
