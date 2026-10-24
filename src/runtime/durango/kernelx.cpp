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
BOOL WINAPI get_thread_name(HANDLE thread, wchar_t *name, SIZE_T capacity, SIZE_T *length) {
    if (!length) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    wchar_t *description = nullptr;
    if (const auto result = GetThreadDescription(thread, &description); FAILED(result)) {
        *length = 0;
        SetLastError(HRESULT_CODE(result));
        return FALSE;
    }
    *length = wcslen(description);
    const bool fits = name && *length < capacity;
    if (fits)
        std::memcpy(name, description, (*length + 1) * sizeof(wchar_t));
    LocalFree(description);
    SetLastError(fits ? ERROR_SUCCESS : ERROR_INSUFFICIENT_BUFFER);
    return fits;
}
// SYSTEMOSVERSIONINFO: the host's version
struct SystemVersion {
    BYTE major, minor;
    WORD build, revision;
};
void WINAPI get_system_os_version(SystemVersion *version) {
    using Get = LONG(WINAPI *)(OSVERSIONINFOW *);
    OSVERSIONINFOW host{sizeof host};
    if (const auto get =
            reinterpret_cast<Get>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        get(&host);
    DWORD revision = 0, size = sizeof revision;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                     RRF_RT_REG_DWORD, nullptr, &revision, &size) != ERROR_SUCCESS)
        revision = 0;
    *version = {static_cast<BYTE>(host.dwMajorVersion), static_cast<BYTE>(host.dwMinorVersion),
                static_cast<WORD>(host.dwBuildNumber), static_cast<WORD>(revision)};
}
// PROCESSOR_SCHEDULING_STATISTICS, in 100 ns units
struct SchedulingStatistics {
    ULONGLONG running, idle, global;
};
void WINAPI query_processor_scheduling_statistics(SchedulingStatistics *statistics) {
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    FILETIME idle, kernel, user;
    const auto value = [](FILETIME time) {
        return ULARGE_INTEGER{{time.dwLowDateTime, time.dwHighDateTime}}.QuadPart;
    };
    *statistics = {0, 0,
                   static_cast<ULONGLONG>(counter.QuadPart / (frequency.QuadPart / 10000000))};
    if (GetSystemTimes(&idle, &kernel, &user))
        *statistics = {value(kernel) - value(idle) + value(user), value(idle), statistics->global};
}
// the console's cores are not the host's: the pool runs wherever the host puts it
BOOL WINAPI set_threadpool_affinity_mask(PTP_POOL, DWORD_PTR) { return TRUE; }
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
    registry.add("kernelx.dll", "SetThreadName", reinterpret_cast<void *>(&set_thread_name));
    registry.add("kernelx.dll", "GetThreadName", reinterpret_cast<void *>(&get_thread_name));
    registry.add("kernelx.dll", "GetSystemOSVersion",
                 reinterpret_cast<void *>(&get_system_os_version));
    registry.add("kernelx.dll", "QueryProcessorSchedulingStatistics",
                 reinterpret_cast<void *>(&query_processor_scheduling_statistics));
    registry.add("kernelx.dll", "SetThreadpoolAffinityMask",
                 reinterpret_cast<void *>(&set_threadpool_affinity_mask));
    for (const auto *name : {"TitleMemoryStatus", "JobTitleMemoryStatus"})
        registry.add("kernelx.dll", name, reinterpret_cast<void *>(&title_memory_status));
    registry.add("kernelx.dll", "CreateFileW", reinterpret_cast<void *>(&create_file_w));
    registry.add("kernelx.dll", "CreateFile2", reinterpret_cast<void *>(&create_file2));
    registry.add("kernelx.dll", "LoadLibraryExW", reinterpret_cast<void *>(&load_library));
    registry.add("kernelx.dll", "GetProcAddress", reinterpret_cast<void *>(&get_proc_address));
    // ETW and address waits are in kernelx on the console; on the desktop kernelbase has
    // them, not kernel32
    if (const auto base = LoadLibraryExW(L"kernelbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        for (const auto *name :
             {"EventRegister", "EventUnregister", "EventWrite", "EventWriteEx", "EventWriteString",
              "EventWriteTransfer", "EventEnabled", "EventProviderEnabled", "EventSetInformation",
              "EventActivityIdControl", "TraceEvent", "WaitOnAddress", "WakeByAddressSingle",
              "WakeByAddressAll"})
            if (const auto function = GetProcAddress(base, name))
                registry.add("kernelx.dll", name, reinterpret_cast<void *>(function));
    install_memory(registry);
}
} // namespace xs::runtime::durango
