#include "host/files.hpp"
#include "host/memory.hpp"
#include "host/module.hpp"
#include "host/threading.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <windows.h>

namespace xs::host {
std::filesystem::path executable_directory() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}
namespace {
void check_path(const std::filesystem::path &path) {
    if (path.empty() || path.native().find('\0') != std::filesystem::path::string_type::npos)
        throw std::invalid_argument("Expected nonempty path without embedded NUL");
}
std::size_t checked_size(std::uintmax_t bytes, std::size_t limit) {
    if (bytes > limit)
        throw std::length_error("File exceeds configured byte limit");
    return static_cast<std::size_t>(bytes);
}
[[noreturn]] void fail(const char *operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}
class Handle {
  public:
    explicit Handle(HANDLE h, const char *operation) : value(h) {
        if (!h || h == INVALID_HANDLE_VALUE)
            fail(operation);
    }
    ~Handle() { CloseHandle(value); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE value;
};
Handle open_input(const std::filesystem::path &path) {
    check_path(path);
    return Handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr),
                  "CreateFileW");
}
std::size_t input_size(const Handle &file, std::size_t limit) {
    if (GetFileType(file.value) != FILE_TYPE_DISK)
        throw std::invalid_argument("Expected a regular host file");
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file.value, &size))
        fail("GetFileSizeEx");
    if (size.QuadPart < 0)
        throw std::runtime_error("Invalid host file size");
    return checked_size(static_cast<std::uintmax_t>(size.QuadPart), limit);
}
} // namespace

std::size_t page_size() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
}
Reservation::Reservation(std::size_t bytes) : page_(page_size()) {
    if (!bytes || bytes > std::numeric_limits<std::size_t>::max() - (page_ - 1))
        throw std::invalid_argument("Invalid host reservation size");
    size_ = ((bytes + page_ - 1) / page_) * page_;
    address_ = static_cast<std::byte *>(VirtualAlloc(nullptr, size_, MEM_RESERVE, PAGE_NOACCESS));
    if (!address_)
        fail("VirtualAlloc reserve");
}
Reservation::~Reservation() { VirtualFree(address_, 0, MEM_RELEASE); }
void Reservation::commit() {
    if (committed_)
        throw std::logic_error("Reservation is already committed");
    if (!VirtualAlloc(address_, size_, MEM_COMMIT, PAGE_READWRITE))
        fail("VirtualAlloc commit");
    committed_ = true;
}
void Reservation::protect(std::size_t offset, std::size_t length, Access access) {
    if (!committed_)
        throw std::logic_error("Reservation is not committed");
    if (!length || offset % page_ || length % page_ || offset > size_ || length > size_ - offset)
        throw std::invalid_argument("Protection requires whole pages inside the reservation");
    DWORD flags, previous;
    switch (access) {
    case Access::none:
        flags = PAGE_NOACCESS;
        break;
    case Access::read:
        flags = PAGE_READONLY;
        break;
    case Access::read_write:
        flags = PAGE_READWRITE;
        break;
    case Access::read_execute:
        flags = PAGE_EXECUTE_READ;
        break;
    default:
        throw std::invalid_argument("Unknown host page access");
    }
    if (!VirtualProtect(address_ + offset, length, flags, &previous))
        fail("VirtualProtect");
}

namespace {
std::vector<std::uint8_t> read_leading(const Handle &file, std::size_t count) {
    std::vector<std::uint8_t> bytes(count);
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        DWORD got;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - pos, 1 << 20));
        if (!ReadFile(file.value, bytes.data() + pos, chunk, &got, nullptr))
            fail("ReadFile");
        if (!got)
            throw std::runtime_error("Host file truncated during read");
        pos += got;
    }
    return bytes;
}
} // namespace
std::vector<std::uint8_t> read_snapshot(const std::filesystem::path &path, std::size_t limit) {
    const auto file = open_input(path);
    return read_leading(file, input_size(file, limit));
}
Prefix read_prefix(const std::filesystem::path &path, std::size_t count) {
    const auto file = open_input(path);
    const auto size = input_size(file, std::numeric_limits<std::size_t>::max());
    return {read_leading(file, std::min(size, count)), size};
}
FileMapping::FileMapping(const std::filesystem::path &path, std::size_t limit) {
    const auto file = open_input(path);
    const auto size = input_size(file, limit);
    if (size) {
        Handle mapping(CreateFileMappingW(file.value, nullptr, PAGE_READONLY, 0, 0, nullptr),
                       "CreateFileMappingW");
        const auto *address = MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, size);
        if (!address)
            fail("MapViewOfFile");
        bytes_ = {static_cast<const std::uint8_t *>(address), size};
    }
}
FileMapping::~FileMapping() {
    if (!bytes_.empty())
        UnmapViewOfFile(bytes_.data());
}

ThreadSlot::ThreadSlot() : key_(TlsAlloc()) {
    if (key_ == TLS_OUT_OF_INDEXES)
        fail("TlsAlloc");
}
ThreadSlot::~ThreadSlot() { TlsFree(key_); }
void ThreadSlot::set(void *value) {
    if (!TlsSetValue(key_, value))
        fail("TlsSetValue");
}
void *ThreadSlot::get() const {
    auto value = TlsGetValue(key_);
    if (!value) {
        if (DWORD error = GetLastError())
            fail("TlsGetValue", error);
    }
    return value;
}
void ManualEvent::signal() {
    {
        std::lock_guard lock(mutex_);
        signaled_ = true;
    }
    changed_.notify_all();
}
void ManualEvent::reset() {
    std::lock_guard lock(mutex_);
    signaled_ = false;
}
bool ManualEvent::wait(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    return changed_.wait(lock, stop, [this] { return signaled_; });
}

NativeModule::NativeModule(const std::filesystem::path &path) {
    check_path(path);
    if (!path.is_absolute())
        throw std::invalid_argument("Trusted host module path must be absolute");
    handle_ = LoadLibraryExW(path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!handle_)
        fail("LoadLibraryExW");
}
NativeModule::~NativeModule() { FreeLibrary(static_cast<HMODULE>(handle_)); }
NativeModule::Function NativeModule::function(const char *name) const {
    if (!name || !*name)
        throw std::invalid_argument("Expected a host function name");
    auto address = GetProcAddress(static_cast<HMODULE>(handle_), name);
    if (!address)
        fail("GetProcAddress");
    return std::bit_cast<Function>(address);
}
} // namespace xs::host
