#pragma once

namespace xs::runtime {
class Registry;
namespace durango {
// Adds the Xbox One console modules (d3d12_x.dll and friends) to the registry.
void install(Registry &registry);
// kernelx.dll functions that are not Win32 ones. Does nothing on hosts that cannot execute.
void install_kernelx(Registry &registry);
// WinRT activation for C++/CX titles. Does nothing on hosts that cannot execute.
void install_winrt(Registry &registry);
// d3d11_x.dll. Only on hosts with a Direct3D 11 of their own.
void install_d3d11x(Registry &registry);
} // namespace durango
} // namespace xs::runtime
