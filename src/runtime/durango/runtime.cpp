#include "runtime/durango/runtime.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/acphal.hpp"
#include "runtime/modules/gameinput.hpp"
#include "runtime/modules/gameruntime.hpp"
#include "runtime/modules/kernel.hpp"
#include "runtime/modules/memory.hpp"
#include "runtime/modules/system.hpp"
#include "runtime/modules/window.hpp"
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
    // what an Xbox One GDK title has in common with a Series one
    modules::install_kernel(registry, Target::durango);
    modules::install_window(registry);
    modules::install_gameruntime(registry);
    modules::install_gameinput(registry);
    modules::install_xmem(registry);
    modules::install_xaudio2(registry);
    modules::install_front_panel(registry);
    modules::install_shader_compiler(registry, "dxcompiler_x.dll");
    install_system(registry);
    modules::install_acphal(registry);
    modules::install_xg(registry, "xg_x.dll");
    install_winrt(registry);
    install_d3d11x(registry);
    for (const auto *name :
         {"EtxRegister", "EtxUnregister", "EtxEventWrite", "EtxFillCommonFields_v7"})
        registry.add("etwplus.dll", name, reinterpret_cast<void *>(&ignored));
}
} // namespace xs::runtime::durango
