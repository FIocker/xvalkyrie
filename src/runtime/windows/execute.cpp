#include "formats/pe/inspect.hpp"
#include "guest/image.hpp"
#include "runtime/runtime.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>
#include <windows.h>

// One guest per process, running on its own thread. Same ABI as the host,
// so an import is just a function pointer.
namespace xs::runtime {
namespace {
struct Guest {
    std::uint64_t base = 0, size = 0;
    std::wstring path;
    std::vector<std::string> missing; // "module!symbol", indexed by stub number
    std::uint8_t *stubs = nullptr;
};
Guest *guest = nullptr;
constexpr std::size_t stub_size = 32, stub_limit = 65536;

bool is_guest(const void *address) {
    const auto value = reinterpret_cast<std::uint64_t>(address);
    return value >= guest->base && value - guest->base < guest->size;
}
HMODULE guest_module() { return reinterpret_cast<HMODULE>(guest->base); }

// The process is xscarlet.exe, but the guest expects to be the main module.
HMODULE WINAPI get_module_handle_w(LPCWSTR name) {
    return name ? GetModuleHandleW(name) : guest_module();
}
HMODULE WINAPI get_module_handle_a(LPCSTR name) {
    return name ? GetModuleHandleA(name) : guest_module();
}
BOOL WINAPI get_module_handle_ex_w(DWORD flags, LPCWSTR name, HMODULE *module) {
    const bool by_address = flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    if (module && (by_address ? is_guest(name) : !name)) {
        *module = guest_module();
        return TRUE;
    }
    return GetModuleHandleExW(flags, name, module);
}
DWORD WINAPI get_module_file_name_w(HMODULE module, LPWSTR buffer, DWORD size) {
    if (module && module != guest_module())
        return GetModuleFileNameW(module, buffer, size);
    if (!buffer || !size) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const auto count = std::min<std::size_t>(guest->path.size(), size - 1);
    std::memcpy(buffer, guest->path.data(), count * sizeof(wchar_t));
    buffer[count] = 0;
    SetLastError(count < guest->path.size() ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS);
    return count < guest->path.size() ? size : static_cast<DWORD>(count);
}
DWORD WINAPI get_module_file_name_a(HMODULE module, LPSTR buffer, DWORD size) {
    if (module && module != guest_module())
        return GetModuleFileNameA(module, buffer, size);
    if (!buffer || !size) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const auto needed =
        WideCharToMultiByte(CP_ACP, 0, guest->path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> text(static_cast<std::size_t>(std::max(needed, 1)));
    WideCharToMultiByte(CP_ACP, 0, guest->path.c_str(), -1, text.data(), needed, nullptr, nullptr);
    const auto length = std::strlen(text.data()), count = std::min<std::size_t>(length, size - 1);
    std::memcpy(buffer, text.data(), count);
    buffer[count] = 0;
    SetLastError(count < length ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS);
    return count < length ? size : static_cast<DWORD>(count);
}
// vcruntime uses RtlPcToFileHeader to find the image base of the thrown type, which
// fails for a manually mapped image and breaks every catch. Raise it ourselves.
[[noreturn]] void WINAPI cxx_throw_exception(void *object, void *throw_info) {
    void *base = nullptr;
    if (throw_info && is_guest(throw_info))
        base = guest_module();
    else if (throw_info)
        RtlPcToFileHeader(throw_info, &base);
    std::uint32_t attributes = 0;
    if (throw_info)
        std::memcpy(&attributes, throw_info, 4);
    const ULONG_PTR parameters[4] = {
        attributes & 8 ? 0x01994000u : 0x19930520u, reinterpret_cast<ULONG_PTR>(object),
        reinterpret_cast<ULONG_PTR>(throw_info), reinterpret_cast<ULONG_PTR>(base)};
    RaiseException(0xE06D7363, EXCEPTION_NONCONTINUABLE, 4, parameters);
    std::abort();
}
struct Override {
    std::string_view symbol;
    void *function;
};
template <class F> void *erase(F *function) { return reinterpret_cast<void *>(function); }
const Override overrides[] = {{"GetModuleHandleW", erase(get_module_handle_w)},
                              {"GetModuleHandleA", erase(get_module_handle_a)},
                              {"GetModuleHandleExW", erase(get_module_handle_ex_w)},
                              {"GetModuleFileNameW", erase(get_module_file_name_w)},
                              {"GetModuleFileNameA", erase(get_module_file_name_a)},
                              {"_CxxThrowException", erase(cxx_throw_exception)}};

[[noreturn]] void unimplemented(std::uint32_t index) {
    std::fprintf(stderr, "xscarlet: title called unimplemented import %s\n",
                 guest->missing.at(index).c_str());
    std::fflush(nullptr);
    ExitProcess(exit_unimplemented_import);
}
// mov ecx, index
// mov rax, &unimplemented
// jmp rax
std::uint64_t make_stub(const std::string &name) {
    if (guest->missing.size() == stub_limit)
        throw std::runtime_error("Too many unimplemented imports");
    const auto index = static_cast<std::uint32_t>(guest->missing.size());
    guest->missing.push_back(name);
    auto *code = guest->stubs + index * stub_size;
    const auto target = reinterpret_cast<std::uint64_t>(&unimplemented);
    code[0] = 0xb9;
    std::memcpy(code + 1, &index, 4);
    code[5] = 0x48;
    code[6] = 0xb8;
    std::memcpy(code + 7, &target, 8);
    code[15] = 0xff;
    code[16] = 0xe0;
    return reinterpret_cast<std::uint64_t>(code);
}

#ifdef _MSC_VER
int report_fault(EXCEPTION_POINTERS *info) {
    const auto *record = info->ExceptionRecord;
    const auto address = reinterpret_cast<std::uint64_t>(record->ExceptionAddress);
    if (is_guest(record->ExceptionAddress))
        std::fprintf(stderr, "xscarlet: guest exception 0x%08lx at image+0x%llx\n",
                     record->ExceptionCode, static_cast<unsigned long long>(address - guest->base));
    else
        std::fprintf(stderr, "xscarlet: guest exception 0x%08lx at host address 0x%llx\n",
                     record->ExceptionCode, static_cast<unsigned long long>(address));
    std::fflush(nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
DWORD WINAPI guest_thread(void *entry) {
#ifdef _MSC_VER
    __try {
        return reinterpret_cast<DWORD (*)()>(entry)();
    } __except (report_fault(GetExceptionInformation())) {
        return exit_guest_fault;
    }
#else
    return reinterpret_cast<DWORD (*)()>(entry)();
#endif
}
struct Stubs {
    Stubs() {
        memory = static_cast<std::uint8_t *>(VirtualAlloc(
            nullptr, stub_size * stub_limit, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!memory)
            throw std::runtime_error("Cannot allocate import stubs");
    }
    ~Stubs() { VirtualFree(memory, 0, MEM_RELEASE); }
    std::uint8_t *memory;
};
} // namespace

bool execution_supported() { return true; }

void *detail::runtime_override(std::string_view module, std::string_view symbol) {
    if (!host_policy_allows(module, symbol))
        return nullptr;
    for (const auto &entry : overrides)
        if (entry.symbol == symbol)
            return entry.function;
    return nullptr;
}

void *detail::host_export(std::string_view module, std::string_view symbol) {
    // file names only, loaded from System32
    if (module.empty() || module.size() > 255 ||
        module.find_first_of("\\/:") != std::string_view::npos)
        return nullptr;
    const std::wstring name(module.begin(), module.end());
    const auto library = LoadLibraryExW(name.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library)
        return nullptr;
    return reinterpret_cast<void *>(GetProcAddress(library, std::string(symbol).c_str()));
}

std::uint32_t execute(const std::filesystem::path &path, std::span<const std::uint8_t> file,
                      const Registry &registry, bool allow_missing) {
    if (guest)
        throw std::logic_error("A guest image is already running in this process");
    const auto pe = pe::inspect(file);
    const auto &contents = pe.at("directory_contents");
    if (pe.at("characteristics").get<std::uint64_t>() & 0x2000)
        throw std::runtime_error("The image is a DLL, not an executable");
    if (!pe.at("entry_rva").get<std::uint64_t>())
        throw std::runtime_error("The image has no entry point");
    if (contents.contains("tls"))
        throw std::runtime_error("Unsupported: the image declares static thread-local storage");
    if (contents.contains("delay_imports"))
        throw std::runtime_error("Unsupported: the image declares delay-load imports");

    Stubs stubs;
    Guest state;
    state.path = std::filesystem::absolute(path).wstring();
    state.stubs = stubs.memory;
    guest = &state;
    struct Reset {
        ~Reset() { guest = nullptr; }
    } reset;

    guest::MapOptions options;
    options.executable = true;
    options.resolve = [&](std::string_view module,
                          std::string_view symbol) -> std::optional<std::uint64_t> {
        void *function = registry.find(module, symbol);
        if (!function)
            function = detail::runtime_override(module, symbol);
        if (!function && detail::host_policy_allows(module, symbol))
            function = detail::host_export(module, symbol);
        if (function)
            return reinterpret_cast<std::uint64_t>(function);
        return make_stub(std::string(module) + "!" + std::string(symbol));
    };
    const guest::MappedImage image(file, options);
    const auto &layout = image.report();
    if (!layout.at("imports").at("modules_without_symbols").empty())
        throw std::runtime_error("Unsupported: the image has a bound import table");
    if (!allow_missing && !state.missing.empty()) {
        std::string text =
            std::to_string(state.missing.size()) + " imports have no implementation:";
        for (std::size_t i = 0; i < std::min<std::size_t>(state.missing.size(), 8); ++i)
            text += " " + state.missing[i];
        throw std::runtime_error(text + (state.missing.size() > 8 ? " ..." : ""));
    }
    DWORD previous;
    if (!VirtualProtect(stubs.memory, stub_size * stub_limit, PAGE_EXECUTE_READ, &previous))
        throw std::runtime_error("Cannot protect import stubs");
    state.base = image.guest_base();
    state.size = layout.at("size_of_image");

    // needed for SEH and C++ exceptions inside the guest
    const auto &directories = pe.at("directory_descriptors");
    std::uint64_t table = 0, count = 0;
    if (contents.contains("exceptions")) {
        table = directories.at(3).at("rva").get<std::uint64_t>();
        count = directories.at(3).at("size").get<std::uint64_t>() / 12;
    }
    auto *const functions = reinterpret_cast<PRUNTIME_FUNCTION>(state.base + table);
    if (count && !RtlAddFunctionTable(functions, static_cast<DWORD>(count), state.base))
        throw std::runtime_error("Cannot register the guest exception table");

    auto *const entry = reinterpret_cast<void *>(layout.at("entry_va").get<std::uint64_t>());
    std::fflush(nullptr);
    const auto thread = CreateThread(nullptr, 0, guest_thread, entry, 0, nullptr);
    if (!thread) {
        if (count)
            RtlDeleteFunctionTable(functions);
        throw std::runtime_error("Cannot create the guest thread");
    }
    WaitForSingleObject(thread, INFINITE);
    DWORD code = exit_guest_fault;
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    if (count)
        RtlDeleteFunctionTable(functions);
    return code;
}
} // namespace xs::runtime
