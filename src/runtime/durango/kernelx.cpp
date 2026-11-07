#include "runtime/common/runtime.hpp"
#include "runtime/durango/d3d11x/placement.hpp"
#include "runtime/durango/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <intrin.h>
#include <shlwapi.h>
#include <string>
#include <string_view>
#include <windows.h>

#include <psapi.h> // after windows.h

namespace xs::runtime::durango {
namespace {
// ours and the title's first, then the host's
HMODULE WINAPI load_library(const wchar_t *name, HANDLE file, DWORD flags) {
    auto module = static_cast<HMODULE>(detail::guest_library(name));
    if (!module)
        module = LoadLibraryExW(name, file, flags);
    const auto error = GetLastError();
    std::fprintf(stderr, "[Runtime] LoadLibraryEx(%ls) -> %p\n", name ? name : L"(null)",
                 static_cast<void *>(module));
    std::fflush(stderr);
    SetLastError(error);
    return module;
}
HMODULE WINAPI load_library_w(const wchar_t *name) { return load_library(name, nullptr, 0); }
HMODULE WINAPI load_packaged_library(const wchar_t *name, DWORD) {
    return load_library(name, nullptr, 0);
}
HMODULE WINAPI load_library_ex_a(const char *name, HANDLE file, DWORD flags) {
    if (!name)
        return load_library(nullptr, file, flags);
    const auto size = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(size, 1)), L'\0');
    MultiByteToWideChar(CP_ACP, 0, name, -1, wide.data(), size);
    return load_library(wide.c_str(), file, flags);
}
HMODULE WINAPI load_library_a(const char *name) { return load_library_ex_a(name, nullptr, 0); }
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
// reads into memory placement resources live in
BOOL WINAPI read_file(HANDLE file, void *buffer, DWORD bytes, DWORD *read, OVERLAPPED *overlapped) {
    d3d11x::prepare_title_write(buffer, bytes);
    return ReadFile(file, buffer, bytes, read, overlapped);
}
BOOL WINAPI read_file_ex(HANDLE file, void *buffer, DWORD bytes, OVERLAPPED *overlapped,
                         LPOVERLAPPED_COMPLETION_ROUTINE completion) {
    d3d11x::prepare_title_write(buffer, bytes);
    return ReadFileEx(file, buffer, bytes, overlapped, completion);
}
FARPROC WINAPI get_proc_address(HMODULE module, const char *name) {
    const bool named = reinterpret_cast<std::uintptr_t>(name) >= 0x10000;
    if (named && !std::strcmp(name, "CreateXAudio2Object"))
        return reinterpret_cast<FARPROC>(&create_xaudio2);
    auto function = reinterpret_cast<FARPROC>(detail::guest_symbol(module, name));
    if (!function)
        function = GetProcAddress(module, name);
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
// The console's drives: G:\ (and the GAME:\ device name) is the title's package, T:\ its
// temporary storage, kept with its saves. Other paths are the host's.
std::wstring console_path(const wchar_t *name) {
    if (!name)
        return {};
    const std::wstring_view path(name);
    const auto drive = [&](std::wstring_view prefix) {
        return path.size() > prefix.size() &&
               CompareStringOrdinal(path.data(), static_cast<int>(prefix.size()), prefix.data(),
                                    static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL &&
               (path[prefix.size()] == L'\\' || path[prefix.size()] == L'/');
    };
    std::error_code error;
    if (drive(L"G:") || drive(L"GAME:")) {
        const auto rest = path.substr(path.find(L':') + 2);
        return (package_directory(std::filesystem::current_path(error)) / rest).wstring();
    }
    if (drive(L"T:")) {
        const auto folder = save_directory() / "Temp";
        std::filesystem::create_directories(folder, error);
        return (folder / path.substr(3)).wstring();
    }
    // from the root of the current drive, which is the package's on the console
    if (path.size() > 1 && (path[0] == L'\\' || path[0] == L'/') && path[1] != L'\\' &&
        path[1] != L'/')
        return (package_directory(std::filesystem::current_path(error)) / path.substr(1)).wstring();
    return std::wstring(path);
}
// the same for the ANSI functions, whose results stay in the title's code page
std::string console_path(const char *name) {
    if (!name)
        return {};
    const auto size = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(size, 1)), L'\0');
    MultiByteToWideChar(CP_ACP, 0, name, -1, wide.data(), size);
    const auto path = console_path(wide.c_str());
    const auto bytes =
        WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(std::max(bytes, 1)), '\0');
    WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, narrow.data(), bytes, nullptr, nullptr);
    narrow.resize(std::strlen(narrow.c_str()));
    return narrow;
}

// XS_FILE_TRACE: every file the title looks for and does not find, to see what content it
// expects where
void note_missing(const char *call, const wchar_t *name, bool failed) {
    static const bool enabled = std::getenv("XS_FILE_TRACE") != nullptr;
    if (!enabled || !failed)
        return;
    const auto error = GetLastError();
    std::fprintf(stderr, "[Runtime] %s(%ls): error %lu\n", call, name ? name : L"(null)", error);
    std::fflush(stderr);
    SetLastError(error);
}
HANDLE WINAPI create_file_w(const wchar_t *name, DWORD access, DWORD share,
                            SECURITY_ATTRIBUTES *security, DWORD creation, DWORD flags,
                            HANDLE model) {
    const auto file =
        CreateFileW(console_path(name).c_str(), access, share, security, creation, flags, model);
    note_music(name, file);
    note_missing("CreateFileW", name, file == INVALID_HANDLE_VALUE);
    return file;
}
HANDLE WINAPI create_file2(const wchar_t *name, DWORD access, DWORD share, DWORD creation,
                           CREATEFILE2_EXTENDED_PARAMETERS *parameters) {
    const auto file = CreateFile2(console_path(name).c_str(), access, share, creation, parameters);
    note_music(name, file);
    note_missing("CreateFile2", name, file == INVALID_HANDLE_VALUE);
    return file;
}
BOOL WINAPI get_file_attributes_ex_w(const wchar_t *name, GET_FILEEX_INFO_LEVELS level,
                                     void *information) {
    const auto found = GetFileAttributesExW(console_path(name).c_str(), level, information);
    note_missing("GetFileAttributesExW", name, !found);
    return found;
}
DWORD WINAPI get_file_attributes_w(const wchar_t *name) {
    const auto attributes = GetFileAttributesW(console_path(name).c_str());
    note_missing("GetFileAttributesW", name, attributes == INVALID_FILE_ATTRIBUTES);
    return attributes;
}
HANDLE WINAPI find_first_file_ex_w(const wchar_t *name, FINDEX_INFO_LEVELS level, void *data,
                                   FINDEX_SEARCH_OPS search, void *filter, DWORD flags) {
    const auto find =
        FindFirstFileExW(console_path(name).c_str(), level, data, search, filter, flags);
    note_missing("FindFirstFileExW", name, find == INVALID_HANDLE_VALUE);
    return find;
}
void note_missing(const char *call, const char *name, bool failed) {
    const auto error = GetLastError();
    const std::wstring wide(name ? name : "", name ? name + std::strlen(name) : "");
    SetLastError(error);
    note_missing(call, wide.c_str(), failed);
}
HANDLE WINAPI create_file_a(const char *name, DWORD access, DWORD share,
                            SECURITY_ATTRIBUTES *security, DWORD creation, DWORD flags,
                            HANDLE model) {
    const auto file =
        CreateFileA(console_path(name).c_str(), access, share, security, creation, flags, model);
    note_missing("CreateFileA", name, file == INVALID_HANDLE_VALUE);
    return file;
}
BOOL WINAPI get_file_attributes_ex_a(const char *name, GET_FILEEX_INFO_LEVELS level,
                                     void *information) {
    const auto found = GetFileAttributesExA(console_path(name).c_str(), level, information);
    note_missing("GetFileAttributesExA", name, !found);
    return found;
}
DWORD WINAPI get_file_attributes_a(const char *name) {
    const auto attributes = GetFileAttributesA(console_path(name).c_str());
    note_missing("GetFileAttributesA", name, attributes == INVALID_FILE_ATTRIBUTES);
    return attributes;
}
HANDLE WINAPI find_first_file_ex_a(const char *name, FINDEX_INFO_LEVELS level, void *data,
                                   FINDEX_SEARCH_OPS search, void *filter, DWORD flags) {
    const auto find =
        FindFirstFileExA(console_path(name).c_str(), level, data, search, filter, flags);
    note_missing("FindFirstFileExA", name, find == INVALID_HANDLE_VALUE);
    return find;
}
HANDLE WINAPI find_first_file_a(const char *name, WIN32_FIND_DATAA *data) {
    const auto find = FindFirstFileA(console_path(name).c_str(), data);
    note_missing("FindFirstFileA", name, find == INVALID_HANDLE_VALUE);
    return find;
}
BOOL WINAPI create_directory_w(const wchar_t *name, SECURITY_ATTRIBUTES *security) {
    return CreateDirectoryW(console_path(name).c_str(), security);
}
BOOL WINAPI remove_directory_w(const wchar_t *name) {
    return RemoveDirectoryW(console_path(name).c_str());
}
BOOL WINAPI delete_file_w(const wchar_t *name) { return DeleteFileW(console_path(name).c_str()); }
BOOL WINAPI move_file_ex_w(const wchar_t *from, const wchar_t *to, DWORD flags) {
    return MoveFileExW(console_path(from).c_str(), to ? console_path(to).c_str() : nullptr, flags);
}
BOOL WINAPI set_file_attributes_w(const wchar_t *name, DWORD attributes) {
    return SetFileAttributesW(console_path(name).c_str(), attributes);
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
BOOL WINAPI set_threadpool_affinity_mask(PTP_POOL, DWORD_PTR) { return TRUE; }
// XS_THREAD_TRACE: where the title puts its threads
DWORD_PTR WINAPI set_thread_affinity_mask(HANDLE thread, DWORD_PTR mask) {
    const auto previous = SetThreadAffinityMask(thread, mask);
    if (std::getenv("XS_THREAD_TRACE")) {
        std::fprintf(stderr, "[Runtime] SetThreadAffinityMask(%lu, 0x%llx) -> 0x%llx from %s\n",
                     GetThreadId(thread), static_cast<unsigned long long>(mask),
                     static_cast<unsigned long long>(previous),
                     detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return previous;
}
DWORD WINAPI get_current_processor_number() {
    const auto number = GetCurrentProcessorNumber();
    if (static std::atomic<int> shown{0}; std::getenv("XS_THREAD_TRACE") && shown++ < 12) {
        std::fprintf(stderr, "[Runtime] GetCurrentProcessorNumber -> %lu from %s\n", number,
                     detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return number;
}
void WINAPI get_system_info(SYSTEM_INFO *info) {
    GetSystemInfo(info);
    if (std::getenv("XS_THREAD_TRACE")) {
        std::fprintf(stderr, "[Runtime] GetSystemInfo: %lu processors, from %s\n",
                     info->dwNumberOfProcessors,
                     detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
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
    registry.add("kernelx.dll", "GetFileAttributesExW",
                 reinterpret_cast<void *>(&get_file_attributes_ex_w));
    registry.add("kernelx.dll", "GetFileAttributesW",
                 reinterpret_cast<void *>(&get_file_attributes_w));
    registry.add("kernelx.dll", "FindFirstFileExW",
                 reinterpret_cast<void *>(&find_first_file_ex_w));
    registry.add("kernelx.dll", "SetThreadAffinityMask",
                 reinterpret_cast<void *>(&set_thread_affinity_mask));
    registry.add("kernelx.dll", "GetCurrentProcessorNumber",
                 reinterpret_cast<void *>(&get_current_processor_number));
    registry.add("kernelx.dll", "GetSystemInfo", reinterpret_cast<void *>(&get_system_info));
    registry.add("kernelx.dll", "CreateFileA", reinterpret_cast<void *>(&create_file_a));
    registry.add("kernelx.dll", "GetFileAttributesExA",
                 reinterpret_cast<void *>(&get_file_attributes_ex_a));
    registry.add("kernelx.dll", "GetFileAttributesA",
                 reinterpret_cast<void *>(&get_file_attributes_a));
    registry.add("kernelx.dll", "FindFirstFileExA",
                 reinterpret_cast<void *>(&find_first_file_ex_a));
    registry.add("kernelx.dll", "FindFirstFileA", reinterpret_cast<void *>(&find_first_file_a));
    registry.add("kernelx.dll", "CreateDirectoryW", reinterpret_cast<void *>(&create_directory_w));
    registry.add("kernelx.dll", "RemoveDirectoryW", reinterpret_cast<void *>(&remove_directory_w));
    registry.add("kernelx.dll", "DeleteFileW", reinterpret_cast<void *>(&delete_file_w));
    registry.add("kernelx.dll", "MoveFileExW", reinterpret_cast<void *>(&move_file_ex_w));
    registry.add("kernelx.dll", "SetFileAttributesW",
                 reinterpret_cast<void *>(&set_file_attributes_w));
    registry.add("kernelx.dll", "LoadLibraryExW", reinterpret_cast<void *>(&load_library));
    registry.add("kernelx.dll", "LoadLibraryW", reinterpret_cast<void *>(&load_library_w));
    registry.add("kernelx.dll", "LoadLibraryExA", reinterpret_cast<void *>(&load_library_ex_a));
    registry.add("kernelx.dll", "LoadLibraryA", reinterpret_cast<void *>(&load_library_a));
    registry.add("kernelx.dll", "LoadPackagedLibrary",
                 reinterpret_cast<void *>(&load_packaged_library));
    registry.add("kernelx.dll", "GetProcAddress", reinterpret_cast<void *>(&get_proc_address));
    registry.add("kernelx.dll", "ReadFile", reinterpret_cast<void *>(&read_file));
    registry.add("kernelx.dll", "ReadFileEx", reinterpret_cast<void *>(&read_file_ex));
    // in kernelbase on the desktop, not kernel32
    if (const auto base = LoadLibraryExW(L"kernelbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        for (const auto *name :
             {"EventRegister", "EventUnregister", "EventWrite", "EventWriteEx", "EventWriteString",
              "EventWriteTransfer", "EventEnabled", "EventProviderEnabled", "EventSetInformation",
              "EventActivityIdControl", "TraceEvent", "WaitOnAddress", "WakeByAddressSingle",
              "WakeByAddressAll"})
            if (const auto function = GetProcAddress(base, name))
                registry.add("kernelx.dll", name, reinterpret_cast<void *>(function));
    // classic ETW, in advapi32 on the desktop
    if (const auto advapi = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        for (const auto *name :
             {"RegisterTraceGuidsW", "RegisterTraceGuidsA", "UnregisterTraceGuids",
              "GetTraceLoggerHandle", "GetTraceEnableLevel", "GetTraceEnableFlags", "TraceMessage",
              "TraceMessageVa", "TraceEventInstance"})
            if (const auto function = GetProcAddress(advapi, name))
                registry.add("kernelx.dll", name, reinterpret_cast<void *>(function));
    install_memory(registry);
}
} // namespace xs::runtime::durango
