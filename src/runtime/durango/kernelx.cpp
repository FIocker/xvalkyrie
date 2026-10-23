#include "runtime/common/runtime.hpp"
#include "runtime/durango/runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <shlwapi.h>
#include <string>
#include <windows.h>

#include <psapi.h> // after windows.h

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
    std::fprintf(stderr, "[Runtime] LoadLibraryEx(%ls) -> %p\n", name ? name : L"(null)",
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
    std::fprintf(stderr, "[Runtime] XAudio2 engine on the host: 0x%08lx\n",
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
    std::fprintf(stderr, "[Runtime] GetProcAddress(%s%s) -> %p\n", ordinal ? "#" : name,
                 ordinal ? std::to_string(reinterpret_cast<std::uintptr_t>(name)).c_str() : "",
                 reinterpret_cast<void *>(function));
    std::fflush(stderr);
    SetLastError(error);
    return function;
}
// Music is streamed from one file per track, so the files a title opens tell which track it
// starts and when. Worth a line each: there are few of them.
void note_music(const wchar_t *name, HANDLE file) {
    if (!name || !StrStrIW(name, L".binka"))
        return;
    const auto error = GetLastError();
    std::fprintf(stderr, "[Runtime] music: opens %ls%s\n", name,
                 file == INVALID_HANDLE_VALUE ? " (failed)" : "");
    std::fflush(stderr);
    SetLastError(error);
}
HANDLE WINAPI create_file_w(const wchar_t *name, DWORD access, DWORD share,
                            SECURITY_ATTRIBUTES *security, DWORD creation, DWORD flags,
                            HANDLE model) {
    const auto file = CreateFileW(name, access, share, security, creation, flags, model);
    note_music(name, file);
    return file;
}
HANDLE WINAPI create_file2(const wchar_t *name, DWORD access, DWORD share, DWORD creation,
                           CREATEFILE2_EXTENDED_PARAMETERS *parameters) {
    const auto file = CreateFile2(name, access, share, creation, parameters);
    note_music(name, file);
    return file;
}
// The console's VirtualAlloc takes more: where the memory goes (MEM_GRAPHICS 0x10000000,
// MEM_TITLE 0x40000000), page sizes the desktop only grants with a privilege, and GPU
// protections above the desktop ones. Here it is all ordinary memory.
constexpr DWORD allocation_types =
    MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_WRITE_WATCH | MEM_RESET_UNDO;
constexpr DWORD protections = 0xff | PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE;
void *WINAPI virtual_alloc_ex(HANDLE process, void *address, SIZE_T size, DWORD type,
                              DWORD protect) {
    const auto result =
        VirtualAllocEx(process, address, size, type & allocation_types, protect & protections);
    if (!result && (type & ~allocation_types || protect & ~protections)) {
        const auto error = GetLastError();
        std::fprintf(stderr,
                     "[Runtime] VirtualAlloc of %zu bytes, type 0x%lx protect 0x%lx failed: %lu\n",
                     static_cast<std::size_t>(size), type, protect, error);
        std::fflush(stderr);
        SetLastError(error);
    }
    return result;
}
void *WINAPI virtual_alloc(void *address, SIZE_T size, DWORD type, DWORD protect) {
    return virtual_alloc_ex(GetCurrentProcess(), address, size, type, protect);
}
BOOL WINAPI virtual_protect(void *address, SIZE_T size, DWORD protect, DWORD *previous) {
    return VirtualProtect(address, size, protect & protections, previous);
}
// What a title writes for a debugger: its own log, which is what says why it gives up.
// Lines go to stderr as they come, without the trailing newline titles usually add.
void WINAPI output_debug_string_w(const wchar_t *text) {
    if (!text)
        return;
    std::wstring line = text;
    while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r'))
        line.pop_back();
    std::fprintf(stderr, "[Title] %ls\n", line.c_str());
    std::fflush(stderr);
}
void WINAPI output_debug_string_a(const char *text) {
    if (!text)
        return;
    std::string line = text;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    std::fprintf(stderr, "[Title] %s\n", line.c_str());
    std::fflush(stderr);
}
// CONSOLE_TYPE: unknown, Xbox One, One S, One X, One X devkit. The most capable retail one,
// which is what titles with an Xbox One X mode look for.
int WINAPI get_console_type() { return 3; }
BOOL WINAPI set_thread_name(HANDLE thread, const wchar_t *name) {
    return SUCCEEDED(SetThreadDescription(thread, name));
}
// TITLEMEMORYSTATUS: the memory of a title, from what the host has
struct TitleMemory {
    DWORD length, reserved;
    ULONGLONG total, available, legacy_used, legacy_peak, legacy_available, title_used,
        title_available, legacy_page_tables, title_page_tables;
};
BOOL WINAPI title_memory_status(TitleMemory *status) {
    if (!status || status->length < sizeof *status) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    MEMORYSTATUSEX host{sizeof host};
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof process;
    if (!GlobalMemoryStatusEx(&host) ||
        !K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&process),
                                 sizeof process))
        return FALSE;
    // what a title gets on an Xbox One X, or less if the host has less to give
    const ULONGLONG budget = std::min<ULONGLONG>(9ull << 30, host.ullTotalPhys);
    const ULONGLONG used = std::min<ULONGLONG>(process.PrivateUsage, budget);
    const ULONGLONG available = std::min<ULONGLONG>(budget - used, host.ullAvailPhys);
    *status = {sizeof *status, 0, budget, available, 0, 0, 0, used, available, 0, 0};
    return TRUE;
}
} // namespace

void install_kernelx(Registry &registry) {
    registry.add("kernelx.dll", "GetConsoleType", reinterpret_cast<void *>(&get_console_type));
    registry.add("kernelx.dll", "OutputDebugStringW",
                 reinterpret_cast<void *>(&output_debug_string_w));
    registry.add("kernelx.dll", "OutputDebugStringA",
                 reinterpret_cast<void *>(&output_debug_string_a));
    registry.add("kernelx.dll", "VirtualAlloc", reinterpret_cast<void *>(&virtual_alloc));
    registry.add("kernelx.dll", "VirtualAllocEx", reinterpret_cast<void *>(&virtual_alloc_ex));
    registry.add("kernelx.dll", "VirtualProtect", reinterpret_cast<void *>(&virtual_protect));
    registry.add("kernelx.dll", "SetThreadName", reinterpret_cast<void *>(&set_thread_name));
    for (const auto *name : {"TitleMemoryStatus", "JobTitleMemoryStatus"})
        registry.add("kernelx.dll", name, reinterpret_cast<void *>(&title_memory_status));
    registry.add("kernelx.dll", "CreateFileW", reinterpret_cast<void *>(&create_file_w));
    registry.add("kernelx.dll", "CreateFile2", reinterpret_cast<void *>(&create_file2));
    registry.add("kernelx.dll", "LoadLibraryExW", reinterpret_cast<void *>(&load_library));
    registry.add("kernelx.dll", "GetProcAddress", reinterpret_cast<void *>(&get_proc_address));
    // ETW is in kernelx on the console; on the desktop kernelbase has it, not kernel32
    if (const auto base = LoadLibraryExW(L"kernelbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        for (const auto *name :
             {"EventRegister", "EventUnregister", "EventWrite", "EventWriteEx", "EventWriteString",
              "EventWriteTransfer", "EventEnabled", "EventProviderEnabled", "EventSetInformation",
              "EventActivityIdControl", "TraceEvent"})
            if (const auto function = GetProcAddress(base, name))
                registry.add("kernelx.dll", name, reinterpret_cast<void *>(function));
    for (const auto *name : {"XMemAllocDefault", "XMemAlloc"})
        registry.add("kernelx.dll", name, reinterpret_cast<void *>(&xmem_alloc));
    for (const auto *name : {"XMemFreeDefault", "XMemFree"})
        registry.add("kernelx.dll", name, reinterpret_cast<void *>(&xmem_free));
}
} // namespace xs::runtime::durango
