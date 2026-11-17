#include <windows.h>
#include "runtime/scarlett/runtime.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/acphal.hpp"
#include "runtime/modules/d3d12x.hpp"
#include "runtime/modules/dstorage.hpp"
#include "runtime/modules/gameinput.hpp"
#include "runtime/modules/gameruntime.hpp"
#include "runtime/modules/kernel.hpp"
#include "runtime/modules/memory.hpp"
#include "runtime/modules/system.hpp"
#include "runtime/modules/watchdog.hpp"
#include "runtime/modules/window.hpp"
#include "runtime/modules/xg.hpp"

namespace xs::runtime::scarlett {
namespace {
HRESULT WINAPI succeeded() { return S_OK; }
} // namespace
// A Series title is a GDK title: a Win32 program on API sets, the Gaming Runtime, and the
// console's Direct3D 12. All of it is shared with Xbox One GDK titles.
void install(Registry &registry) {
    modules::frame_watchdog();
    modules::install_kernel(registry, Target::scarlett);
    modules::install_window(registry);
    modules::install_gameruntime(registry);
    modules::install_gameinput(registry);
    modules::install_xmem(registry);
    modules::install_acphal(registry);
    modules::install_xaudio2(registry);
    modules::install_front_panel(registry);
    registry.forward("ws2_32.dll", "ws2_32.dll");
    // Media Foundation is the host's, but for what ties it to the console's graphics device
    registry.forward("mfplat.dll", "mfplat.dll");
    registry.add("mfplat.dll", "MFResetDXGIDeviceManagerX", reinterpret_cast<void *>(&succeeded));
    modules::install_dstorage(registry);
    modules::install_shader_compiler(registry, "dxcompiler_xs.dll");
    modules::install_xg(registry, "xg_xs.dll");
    modules::d3d12x::install(registry, "d3d12_xs.dll", Target::scarlett);
}
} // namespace xs::runtime::scarlett
