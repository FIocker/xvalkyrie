#include "runtime/modules/system.hpp"
#include "host/files.hpp"
#include "runtime/common/runtime.hpp"
#include <cstdio>
#include <windows.h>

namespace xs::runtime::modules {
namespace {
HMODULE host_xaudio2() {
    static const auto library =
        LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return library;
}
// The processor argument names console cores, so the host's default is used instead
HRESULT __cdecl xaudio2_create(void **engine, UINT32 flags, UINT32) {
    using Create = HRESULT(__cdecl *)(void **, UINT32, UINT32);
    const auto create = host_xaudio2() ? reinterpret_cast<Create>(
                                             GetProcAddress(host_xaudio2(), "XAudio2Create"))
                                       : nullptr;
    const auto result = create ? create(engine, flags & 0x8000, 1) : E_NOTIMPL;
    std::fprintf(stderr, "[Runtime] XAudio2 engine on the host: 0x%08lx\n",
                 static_cast<unsigned long>(result));
    std::fflush(stderr);
    return result;
}
HRESULT WINAPI dxc_create_instance(const GUID *class_id, const GUID *iid, void **object) {
    using Create = HRESULT(WINAPI *)(const GUID *, const GUID *, void **);
    static const auto create = [] {
        auto library =
            LoadLibraryExW((host::executable_directory() / "dxcompiler.dll").c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library)
            library = LoadLibraryExW(L"dxcompiler.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library)
            std::fprintf(stderr, "[Runtime] the title asks for the shader compiler, and there "
                                 "is no dxcompiler.dll next to xscarlet or in the system\n");
        return library ? reinterpret_cast<Create>(GetProcAddress(library, "DxcCreateInstance"))
                       : nullptr;
    }();
    if (object)
        *object = nullptr;
    return create ? create(class_id, iid, object) : CLASS_E_CLASSNOTAVAILABLE;
}
} // namespace

void install_xaudio2(Registry &registry) {
    const char *const module = "xaudio2_9.dll";
    registry.add(module, "XAudio2Create", reinterpret_cast<void *>(&xaudio2_create));
    registry.add(module, "#1", reinterpret_cast<void *>(&xaudio2_create));
    // the console's ordinals for what the host exports by name
    const std::pair<const char *, const char *> named[] = {{"#2", "CreateAudioReverb"},
                                                           {"#3", "CreateAudioVolumeMeter"},
                                                           {"#4", "CreateFX"},
                                                           {"#5", "X3DAudioCalculate"},
                                                           {"#6", "X3DAudioInitialize"},
                                                           {"#7", "CreateAudioReverbV2_8"}};
    for (const auto &[ordinal, name] : named)
        if (auto *const function =
                host_xaudio2() ? GetProcAddress(host_xaudio2(), name) : nullptr) {
            registry.add(module, ordinal, reinterpret_cast<void *>(function));
            registry.add(module, name, reinterpret_cast<void *>(function));
        }
}
void install_shader_compiler(Registry &registry, std::string_view module) {
    registry.add(module, "DxcCreateInstance", reinterpret_cast<void *>(&dxc_create_instance));
}
} // namespace xs::runtime::modules
