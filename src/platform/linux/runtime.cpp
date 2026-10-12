#include "platform/host/detail.hpp"
#include "platform/host/files.hpp"
#include "platform/host/module.hpp"
#include "platform/host/threading.hpp"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace xs::host {
namespace {
[[noreturn]] void fail(const char *operation, int error = errno) {
    throw std::system_error(error, std::generic_category(), operation);
}
class Input {
  public:
    explicit Input(const std::filesystem::path &path) {
        detail::check_path(path);
        fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0)
            fail("open");
    }
    ~Input() { close(fd); }
    Input(const Input &) = delete;
    Input &operator=(const Input &) = delete;
    std::size_t size(std::size_t limit) const {
        struct stat info{};
        if (fstat(fd, &info) != 0)
            fail("fstat");
        if (!S_ISREG(info.st_mode) || info.st_size < 0)
            throw std::invalid_argument("Expected a regular host file");
        return detail::checked_size(static_cast<std::uintmax_t>(info.st_size), limit);
    }
    int fd;
};
} // namespace
std::size_t page_size() {
    const long size = sysconf(_SC_PAGESIZE);
    if (size <= 0)
        throw std::runtime_error("Cannot determine host page size");
    return static_cast<std::size_t>(size);
}
std::byte *detail::reserve(std::size_t size) {
    auto address = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED)
        fail("mmap reservation");
    return static_cast<std::byte *>(address);
}
void detail::release(std::byte *address, std::size_t size) noexcept { munmap(address, size); }
void detail::commit(std::byte *address, std::size_t size) {
    detail::protect(address, size, Access::read_write);
}
void detail::protect(std::byte *address, std::size_t size, Access access) {
    int flags;
    switch (access) {
    case Access::none:
        flags = PROT_NONE;
        break;
    case Access::read:
        flags = PROT_READ;
        break;
    case Access::read_write:
        flags = PROT_READ | PROT_WRITE;
        break;
    case Access::read_execute:
        flags = PROT_READ | PROT_EXEC;
        break;
    default:
        throw std::invalid_argument("Unknown host page access");
    }
    if (mprotect(address, size, flags) != 0)
        fail("mprotect");
}

namespace {
std::vector<std::uint8_t> read_leading(const Input &file, std::size_t count) {
    std::vector<std::uint8_t> bytes(count);
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const auto n =
            read(file.fd, bytes.data() + pos, std::min<std::size_t>(bytes.size() - pos, 1 << 20));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            fail("read");
        if (!n)
            throw std::runtime_error("Host file truncated during read");
        pos += static_cast<std::size_t>(n);
    }
    return bytes;
}
} // namespace
std::vector<std::uint8_t> read_snapshot(const std::filesystem::path &path, std::size_t limit) {
    const Input file(path);
    return read_leading(file, file.size(limit));
}
Prefix read_prefix(const std::filesystem::path &path, std::size_t count) {
    const Input file(path);
    const auto size = file.size(std::numeric_limits<std::size_t>::max());
    return {read_leading(file, std::min(size, count)), size};
}
FileMapping::FileMapping(const std::filesystem::path &path, std::size_t limit) {
    const Input file(path);
    const auto size = file.size(limit);
    if (size) {
        auto address = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file.fd, 0);
        if (address == MAP_FAILED)
            fail("mmap file");
        bytes_ = {static_cast<const std::uint8_t *>(address), size};
    }
}
FileMapping::~FileMapping() {
    if (!bytes_.empty())
        munmap(const_cast<std::uint8_t *>(bytes_.data()), bytes_.size());
}

ThreadSlot::ThreadSlot() {
    pthread_key_t key;
    if (int error = pthread_key_create(&key, nullptr))
        fail("pthread_key_create", error);
    key_ = key;
}
ThreadSlot::~ThreadSlot() { pthread_key_delete(key_); }
void ThreadSlot::set(void *value) {
    if (int error = pthread_setspecific(key_, value))
        fail("pthread_setspecific", error);
}
void *ThreadSlot::get() const { return pthread_getspecific(key_); }

NativeModule::NativeModule(const std::filesystem::path &path) {
    detail::check_module_path(path);
    handle_ = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (!handle_)
        throw std::runtime_error(dlerror());
}
NativeModule::~NativeModule() { dlclose(handle_); }
NativeModule::Function NativeModule::function(const char *name) const {
    if (!name || !*name)
        throw std::invalid_argument("Expected a host function name");
    dlerror();
    auto address = dlsym(handle_, name);
    if (const char *error = dlerror())
        throw std::runtime_error(error);
    if (!address)
        throw std::runtime_error("Host function has no address");
    return std::bit_cast<Function>(address);
}
} // namespace xs::host
