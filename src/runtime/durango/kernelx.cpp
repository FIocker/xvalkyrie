#include "runtime/common/runtime.hpp"
#include "runtime/durango/runtime.hpp"

#ifdef _WIN32
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace xs::runtime::durango {
namespace {
// The attributes ask for an alignment and a page size. A fresh 64K-aligned, zeroed region
// satisfies any of them, at the cost of address space for small blocks.
void *WINAPI xmem_alloc(SIZE_T size, ULONGLONG) {
    return VirtualAlloc(nullptr, size ? size : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
void WINAPI xmem_free(void *address, ULONGLONG) {
    if (address)
        VirtualFree(address, 0, MEM_RELEASE);
}
// Libraries a title loads by name at run time come from the host for now. What it asks
// for and gets is worth knowing.
HMODULE WINAPI load_library(const wchar_t *name, HANDLE file, DWORD flags) {
    const auto module = LoadLibraryExW(name, file, flags);
    const auto error = GetLastError();
    std::fprintf(stderr, "xscarlet: LoadLibraryEx(%ls) -> %p\n", name ? name : L"(null)",
                 static_cast<void *>(module));
    std::fflush(stderr);
    SetLastError(error);
    return module;
}
// xaudio2_9.dll: the console's creates the engine through CreateXAudio2Object, the host's
// through XAudio2Create. The engine and its voices are the same interfaces. The processor
// argument names console cores, so the host's default is used instead.
HRESULT __cdecl create_xaudio2(void **engine, UINT32, UINT32, void *) {
    using Create = HRESULT(__cdecl *)(void **, UINT32, UINT32);
    const auto library = LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create =
        library ? reinterpret_cast<Create>(GetProcAddress(library, "XAudio2Create")) : nullptr;
    const auto result = create ? create(engine, 0, 1) : E_NOTIMPL;
    std::fprintf(stderr, "xscarlet: XAudio2 engine on the host: 0x%08lx\n",
                 static_cast<unsigned long>(result));
    std::fflush(stderr);
    return result;
}
FARPROC WINAPI get_proc_address(HMODULE module, const char *name) {
    const bool named = reinterpret_cast<std::uintptr_t>(name) >= 0x10000;
    if (named && !std::strcmp(name, "CreateXAudio2Object"))
        return reinterpret_cast<FARPROC>(&create_xaudio2);
    const auto function = GetProcAddress(module, name);
    const auto error = GetLastError();
    const bool ordinal = reinterpret_cast<std::uintptr_t>(name) < 0x10000;
    std::fprintf(stderr, "xscarlet: GetProcAddress(%s%s) -> %p\n", ordinal ? "#" : name,
                 ordinal ? std::to_string(reinterpret_cast<std::uintptr_t>(name)).c_str() : "",
                 reinterpret_cast<void *>(function));
    std::fflush(stderr);
    SetLastError(error);
    return function;
}
} // namespace

void install_kernelx(Registry &registry) {
    registry.add("kernelx.dll", "LoadLibraryExW", reinterpret_cast<void *>(&load_library));
    registry.add("kernelx.dll", "GetProcAddress", reinterpret_cast<void *>(&get_proc_address));
    for (const auto *name : {"XMemAllocDefault", "XMemAlloc"})
        registry.add("kernelx.dll", name, reinterpret_cast<void *>(&xmem_alloc));
    for (const auto *name : {"XMemFreeDefault", "XMemFree"})
        registry.add("kernelx.dll", name, reinterpret_cast<void *>(&xmem_free));
}
} // namespace xs::runtime::durango
#else
namespace xs::runtime::durango {
void install_kernelx(Registry &) {}
} // namespace xs::runtime::durango
#endif
