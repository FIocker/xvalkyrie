#include "host/files.hpp"
#include "host/memory.hpp"
#include "host/module.hpp"
#include "host/threading.hpp"
#include <bit>
#include <catch2/catch.hpp>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using namespace xs::host;

// Query the kernel's effective protections rather than deliberately faulting the runner.
std::string page_permissions(const void *address) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)))
        throw std::runtime_error("VirtualQuery failed");
    if (info.State == MEM_RESERVE || info.Protect == PAGE_NOACCESS)
        return "---";
    if (info.Protect == PAGE_READWRITE)
        return "rw-";
    if (info.Protect == PAGE_READONLY)
        return "r--";
#else
    const auto pointer = reinterpret_cast<std::uintptr_t>(address);
    std::ifstream maps("/proc/self/maps");
    for (std::string line; std::getline(maps, line);) {
        std::istringstream row(line);
        std::uintptr_t first, last;
        char dash;
        std::string permissions;
        if (row >> std::hex >> first >> dash >> last >> permissions && dash == '-' &&
            first <= pointer && pointer < last)
            return permissions.substr(0, 3);
    }
#endif
    return "unknown";
}

TEST_CASE("memory reservation", "[host]") {
    const auto page = page_size();
    Reservation memory(page + 1);
    REQUIRE(memory.size() == 2 * page);
    REQUIRE(page_permissions(memory.address()) == "---");
    CHECK_THROWS_AS(memory.protect(0, page, Access::read), std::logic_error);
    memory.commit();
    REQUIRE(page_permissions(memory.address()) == "rw-");
    CHECK(memory.address()[0] == std::byte{0});
    memory.address()[0] = std::byte{42};
    memory.address()[page] = std::byte{17};
    memory.protect(0, page, Access::read);
    CHECK(page_permissions(memory.address()) == "r--");
    CHECK(page_permissions(memory.address() + page) == "rw-");
    memory.protect(0, page, Access::none);
    CHECK(page_permissions(memory.address()) == "---");
    CHECK_THROWS_AS(memory.commit(), std::logic_error);
    memory.protect(0, page, Access::read_write);
    CHECK(memory.address()[0] == std::byte{42});
    CHECK_THROWS_AS(memory.protect(1, page, Access::none), std::invalid_argument);
    CHECK_THROWS_AS(memory.protect(page, 2 * page, Access::read), std::invalid_argument);
    CHECK_THROWS_AS(memory.protect(0, 0, Access::read), std::invalid_argument);
    CHECK_THROWS_AS(Reservation(0), std::invalid_argument);
    CHECK_THROWS_AS(Reservation(std::numeric_limits<std::size_t>::max()), std::invalid_argument);
}

TEST_CASE("file snapshot and mapping", "[host]") {
    auto snapshot = read_snapshot("host-input.bin", 12);
    FileMapping mapping("host-input.bin", 12);
    const auto view = mapping.bytes();
    REQUIRE(std::string(snapshot.begin(), snapshot.end()) == "host runtime");
    REQUIRE(view.size() == snapshot.size());
    REQUIRE(page_permissions(view.data()) == "r--");
    snapshot[0] = 'X';
    CHECK(view[0] == 'h');
    CHECK(FileMapping("host-empty.bin", 0).bytes().empty());
    CHECK(read_snapshot("host-empty.bin", 0).empty());
    CHECK_THROWS_AS(read_snapshot("host-input.bin", 11), std::length_error);
    CHECK_THROWS_AS(FileMapping("host-input.bin", 11), std::length_error);
    CHECK_THROWS(read_snapshot(".", 1024));
    CHECK_THROWS(FileMapping("missing-runtime-fixture.bin", 1024));
}

TEST_CASE("thread slots and stop tokens", "[host]") {
    ThreadSlot slot;
    int main_value = 10, worker_value = 20;
    slot.set(&main_value);
    ManualEvent ready, finish;
    bool initially_empty = false, isolated = false, canceled = false;
    std::jthread worker([&](std::stop_token stop) {
        initially_empty = slot.get() == nullptr;
        slot.set(&worker_value);
        ready.signal();
        canceled = !finish.wait(stop);
        isolated = slot.get() == &worker_value;
    });
    ready.wait();
    const bool main_unchanged = slot.get() == &main_value;
    worker.request_stop();
    worker.join();
    CHECK(main_unchanged);
    CHECK(initially_empty);
    CHECK(isolated);
    CHECK(canceled);
}

TEST_CASE("manual event", "[host]") {
    ManualEvent event;
    std::stop_source canceled;
    canceled.request_stop();
    CHECK_FALSE(event.wait(canceled.get_token()));
    event.signal();
    CHECK(event.wait(canceled.get_token()));
    CHECK(event.wait(canceled.get_token()));
    event.reset();
    CHECK_FALSE(event.wait(canceled.get_token()));
}

TEST_CASE("native module loading", "[host]") {
    NativeModule module(XS_TEST_MODULE);
    const auto probe = std::bit_cast<int (*)()>(module.function("xs_host_test_value"));
    CHECK(probe() == 42);
    CHECK_THROWS(module.function("xs_no_such_export"));
    CHECK_THROWS_AS(NativeModule("relative-library"), std::invalid_argument);
    CHECK_THROWS(NativeModule(std::filesystem::absolute("host-input.bin")));
}
} // namespace
