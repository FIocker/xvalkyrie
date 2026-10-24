#include "runtime/common/static_tls.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

#include <tlhelp32.h> // after windows.h

namespace xs::runtime {
namespace {
constexpr std::uint64_t dll_base = 0x180000000;
constexpr DWORD section_rva = 0x1000, file_alignment = 0x200, section_alignment = 0x1000;
constexpr std::size_t align(std::size_t value, std::size_t to) {
    return (value + to - 1) / to * to;
}
template <class T> void put(std::vector<std::uint8_t> &bytes, std::size_t at, const T &value) {
    std::memcpy(bytes.data() + at, &value, sizeof value);
}

// One section with the TLS directory, the index the loader fills in, the callbacks, the
// template, the relocations of the directory's addresses and one import. The import is what
// makes the system set up the TLS: it does that while it binds a DLL's imports, and skips
// it for a DLL that has none. A second section holds an entry point that returns TRUE:
// without one, the TLS callbacks never see threads attach.
std::vector<std::uint8_t> make_dll(const std::uint8_t *data, std::size_t size,
                                   std::uint32_t zero_fill, std::uint32_t characteristics,
                                   const std::vector<std::uint64_t> &callbacks) {
    constexpr std::size_t index_at = sizeof(IMAGE_TLS_DIRECTORY64), callbacks_at = 0x30;
    const std::size_t template_at = align(callbacks_at + 8 * (callbacks.size() + 1), 16);
    const std::size_t relocations_at = align(template_at + size, 4);
    constexpr std::size_t relocations_size = sizeof(IMAGE_BASE_RELOCATION) + 4 * sizeof(WORD);
    // kernel32!GetTickCount: descriptors (the last one empty), lookup and address tables
    constexpr char function[] = "\0\0GetTickCount", library[] = "kernel32.dll";
    const std::size_t imports_at = align(relocations_at + relocations_size, 8);
    const std::size_t lookup_at = imports_at + 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR);
    const std::size_t addresses_at = lookup_at + 16, function_at = addresses_at + 16;
    const std::size_t library_at = align(function_at + sizeof function, 2);
    const std::size_t content = library_at + sizeof library;
    const std::size_t raw = align(content, file_alignment);
    const auto code_rva = static_cast<DWORD>(section_rva + align(content, section_alignment));
    constexpr std::uint8_t entry[] = {0xb8, 1, 0, 0, 0, 0xc3}; // mov eax, 1; ret

    std::vector<std::uint8_t> bytes(file_alignment + raw + file_alignment);
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = sizeof dos;
    put(bytes, 0, dos);

    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections = 2;
    nt.FileHeader.SizeOfOptionalHeader = sizeof nt.OptionalHeader;
    nt.FileHeader.Characteristics =
        IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE | IMAGE_FILE_DLL;
    auto &optional = nt.OptionalHeader;
    optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    optional.SizeOfInitializedData = static_cast<DWORD>(raw);
    optional.SizeOfCode = file_alignment;
    optional.AddressOfEntryPoint = optional.BaseOfCode = code_rva;
    optional.ImageBase = dll_base;
    optional.SectionAlignment = section_alignment;
    optional.FileAlignment = file_alignment;
    optional.MajorOperatingSystemVersion = optional.MajorSubsystemVersion = 6;
    optional.SizeOfImage = code_rva + section_alignment;
    optional.SizeOfHeaders = file_alignment;
    optional.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
    optional.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA |
                                  IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE |
                                  IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
    optional.SizeOfStackReserve = optional.SizeOfHeapReserve = 0x100000;
    optional.SizeOfStackCommit = optional.SizeOfHeapCommit = 0x1000;
    optional.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS] = {section_rva,
                                                         sizeof(IMAGE_TLS_DIRECTORY64)};
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {
        static_cast<DWORD>(section_rva + imports_at),
        static_cast<DWORD>(2 * sizeof(IMAGE_IMPORT_DESCRIPTOR))};
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT] = {
        static_cast<DWORD>(section_rva + addresses_at), 16};
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] = {
        static_cast<DWORD>(section_rva + relocations_at), static_cast<DWORD>(relocations_size)};
    put(bytes, sizeof dos, nt);

    IMAGE_SECTION_HEADER section{};
    std::memcpy(section.Name, ".tls", 4);
    section.Misc.VirtualSize = static_cast<DWORD>(content);
    section.VirtualAddress = section_rva;
    section.SizeOfRawData = static_cast<DWORD>(raw);
    section.PointerToRawData = file_alignment;
    section.Characteristics =
        IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
    put(bytes, sizeof dos + sizeof nt, section);
    IMAGE_SECTION_HEADER code{};
    std::memcpy(code.Name, ".text", 5);
    code.Misc.VirtualSize = sizeof entry;
    code.VirtualAddress = code_rva;
    code.SizeOfRawData = file_alignment;
    code.PointerToRawData = static_cast<DWORD>(file_alignment + raw);
    code.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
    put(bytes, sizeof dos + sizeof nt + sizeof section, code);
    std::memcpy(bytes.data() + file_alignment + raw, entry, sizeof entry);

    const auto va = [](std::size_t offset) { return dll_base + section_rva + offset; };
    auto *const body = bytes.data() + file_alignment;
    IMAGE_TLS_DIRECTORY64 directory{};
    directory.StartAddressOfRawData = va(template_at);
    directory.EndAddressOfRawData = va(template_at + size);
    directory.AddressOfIndex = va(index_at);
    directory.AddressOfCallBacks = va(callbacks_at);
    directory.SizeOfZeroFill = zero_fill;
    directory.Characteristics = characteristics;
    std::memcpy(body, &directory, sizeof directory);
    // the callbacks are the image's, already where they will run: not relocated
    std::memcpy(body + callbacks_at, callbacks.data(), 8 * callbacks.size());
    if (size)
        std::memcpy(body + template_at, data, size);
    IMAGE_IMPORT_DESCRIPTOR import{};
    import.OriginalFirstThunk = static_cast<DWORD>(section_rva + lookup_at);
    import.Name = static_cast<DWORD>(section_rva + library_at);
    import.FirstThunk = static_cast<DWORD>(section_rva + addresses_at);
    std::memcpy(body + imports_at, &import, sizeof import);
    const std::uint64_t by_name = section_rva + function_at;
    std::memcpy(body + lookup_at, &by_name, sizeof by_name);
    std::memcpy(body + addresses_at, &by_name, sizeof by_name);
    std::memcpy(body + function_at, function, sizeof function);
    std::memcpy(body + library_at, library, sizeof library);
    const IMAGE_BASE_RELOCATION block{section_rva, static_cast<DWORD>(relocations_size)};
    std::memcpy(body + relocations_at, &block, sizeof block);
    for (WORD i = 0; i < 4; ++i) {
        const WORD entry = static_cast<WORD>(IMAGE_REL_BASED_DIR64 << 12 | (i * 8));
        std::memcpy(body + relocations_at + sizeof block + i * sizeof entry, &entry, sizeof entry);
    }
    return bytes;
}

std::wstring temporary_folder() {
    wchar_t buffer[MAX_PATH + 1];
    const auto length = GetTempPathW(MAX_PATH + 1, buffer);
    if (!length || length > MAX_PATH)
        throw std::runtime_error("No temporary folder for the TLS module");
    return std::wstring(buffer, length);
}
// Executables built with /GA read their TLS at index 0, xscarlet.exe's. The title takes it
// and xscarlet.exe moves to the module's index, in every thread: those that exist with the
// rest suspended, new ones from the module's TLS callback.
ULONG title_index = 0;
std::atomic<bool> traded{false};
void **tls_array(void *teb) {
    return *reinterpret_cast<void ***>(static_cast<std::uint8_t *>(teb) + 0x58);
}
void trade(void *teb) {
    if (auto **const blocks = tls_array(teb))
        std::swap(blocks[0], blocks[title_index]);
}
void NTAPI on_thread(void *, DWORD reason, void *) {
    if (reason == DLL_THREAD_ATTACH && traded)
        trade(NtCurrentTeb());
}
ULONG *host_index() {
    const auto *const image = reinterpret_cast<const std::uint8_t *>(GetModuleHandleW(nullptr));
    const auto *const nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
        image + reinterpret_cast<const IMAGE_DOS_HEADER *>(image)->e_lfanew);
    const auto &entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!entry.VirtualAddress)
        return nullptr;
    const auto *const directory =
        reinterpret_cast<const IMAGE_TLS_DIRECTORY64 *>(image + entry.VirtualAddress);
    return reinterpret_cast<ULONG *>(directory->AddressOfIndex);
}
bool give_index_zero(ULONG index) {
    auto *const host = host_index();
    if (!host || *host != 0 || !index)
        return false;
    using Query = LONG(NTAPI *)(HANDLE, int, void *, ULONG, ULONG *);
    const auto query = reinterpret_cast<Query>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    struct BasicInformation {
        LONG status;
        void *teb;
        void *client[2];
        ULONG_PTR affinity;
        LONG priority, base_priority;
    };
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (!query || snapshot == INVALID_HANDLE_VALUE)
        return false;
    std::vector<HANDLE> threads;
    THREADENTRY32 entry{sizeof entry};
    for (auto more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        if (entry.th32OwnerProcessID == GetCurrentProcessId() &&
            entry.th32ThreadID != GetCurrentThreadId())
            if (const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                                               FALSE, entry.th32ThreadID))
                threads.push_back(thread);
    CloseHandle(snapshot);
    for (const auto thread : threads)
        SuspendThread(thread);
    title_index = index;
    for (const auto thread : threads) {
        BasicInformation information{};
        if (query(thread, 0, &information, sizeof information, nullptr) >= 0 && information.teb)
            trade(information.teb);
    }
    trade(NtCurrentTeb());
    *host = index;
    traded = true;
    for (const auto thread : threads) {
        ResumeThread(thread);
        CloseHandle(thread);
    }
    return true;
}

// The modules of earlier runs; the ones still loaded somewhere cannot go and stay.
void remove_old(const std::wstring &folder) {
    WIN32_FIND_DATAW found;
    const auto search = FindFirstFileW((folder + L"xscarlet-tls-*.dll").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do
        DeleteFileW((folder + found.cFileName).c_str());
    while (FindNextFileW(search, &found));
    FindClose(search);
}
} // namespace

void install_static_tls(const nlohmann::json &tls, std::uint64_t base, std::uint64_t image_base,
                        bool executable) {
    const auto at = [&](const char *key) {
        return base + (tls.at(key).get<std::uint64_t>() - image_base);
    };
    const auto start = at("template_start_va"), end = at("template_end_va");
    if (end < start)
        throw std::runtime_error("The TLS template ends before it starts");
    std::vector<std::uint64_t> callbacks;
    if (executable)
        callbacks.push_back(reinterpret_cast<std::uint64_t>(&on_thread));
    for (const auto &callback : tls.at("callbacks_va"))
        callbacks.push_back(base + (callback.get<std::uint64_t>() - image_base));
    const auto bytes = make_dll(reinterpret_cast<const std::uint8_t *>(start), end - start,
                                tls.at("zero_fill").get<std::uint32_t>(),
                                tls.at("characteristics").get<std::uint32_t>(), callbacks);

    const auto folder = temporary_folder();
    remove_old(folder);
    static unsigned count = 0;
    const auto path = folder + L"xscarlet-tls-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(count++) + L".dll";
    {
        std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file)
            throw std::runtime_error("Cannot write the TLS module");
    }
    // stays loaded for as long as the process: every thread's block belongs to it
    const auto module = LoadLibraryExW(path.c_str(), nullptr, 0);
    if (!module)
        throw std::runtime_error("The system did not load the TLS module (error " +
                                 std::to_string(GetLastError()) + ")");
    ULONG index = 0;
    std::memcpy(&index,
                reinterpret_cast<const std::uint8_t *>(module) + section_rva +
                    sizeof(IMAGE_TLS_DIRECTORY64),
                sizeof index);
    const auto loaded = index;
    if (executable && !traded && give_index_zero(index))
        index = 0;
    else if (executable)
        std::fprintf(stderr, "[Runtime] static TLS: index 0 stays xscarlet's\n");
    auto *const slot = reinterpret_cast<ULONG *>(at("index_va"));
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof index, PAGE_READWRITE, &previous))
        throw std::runtime_error("Cannot write the TLS index of the image");
    *slot = index;
    VirtualProtect(slot, sizeof index, previous, &previous);
    std::fprintf(stderr, "[Runtime] static TLS: %llu bytes, %zu callbacks, index %lu%s\n",
                 static_cast<unsigned long long>(end - start), callbacks.size(), index,
                 index != loaded ? " (traded with xscarlet's)" : "");
}
} // namespace xs::runtime
