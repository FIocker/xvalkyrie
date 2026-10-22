#include "runtime/durango/runtime.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/xg.hpp"
#include <cstdint>
#include <initializer_list>

namespace xs::runtime::durango {
namespace {
// Never called with anything we use and returns "success" for every signature.
std::uint64_t ignored() { return 0; }
} // namespace

void install(Registry &registry) {
    // kernelx.dll is the ERA kernel32: Win32 names plus a few of its own (kernelx.cpp)
    registry.forward("kernelx.dll", "kernel32.dll");
    // ERA titles are WinRT applications and use more of these than the GDK allows
    registry.forward("combase.dll", "combase.dll");
    registry.forward("ws2_32.dll", "ws2_32.dll");
    // the shader compiler is the desktop one
    registry.forward("d3dcompiler_47.dll", "d3dcompiler_47.dll");
    install_kernelx(registry);
    modules::install_xg(registry, "xg_x.dll");
    install_winrt(registry);
#ifdef _WIN32
    install_d3d11x(registry);
#endif
    for (const auto *name :
         {"EtxRegister", "EtxUnregister", "EtxEventWrite", "EtxFillCommonFields_v7"})
        registry.add("etwplus.dll", name, reinterpret_cast<void *>(&ignored));
}
} // namespace xs::runtime::durango
