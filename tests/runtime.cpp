#include "runtime/runtime.hpp"
#include <catch2/catch.hpp>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <vector>

namespace {
int probe() { return 1; }
std::vector<std::uint8_t> file(const char *name) {
    std::ifstream input(name, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
} // namespace

TEST_CASE("registry lookup") {
    xs::runtime::Registry registry;
    auto *const function = reinterpret_cast<void *>(&probe);
    registry.add("Fixture.DLL", "Probe", function);
    registry.add("fixture.dll", "#17", function);
    CHECK(registry.find("FIXTURE.dll", "Probe") == function);
    CHECK(registry.find("fixture.dll", "probe") == nullptr);
    CHECK(registry.find("other.dll", "Probe") == nullptr);
    CHECK_THROWS(registry.add("fixture.dll", "Probe", function));
    CHECK_THROWS(registry.add("fixture.dll", "Other", nullptr));
    CHECK_THROWS(registry.add("", "Other", function));
}

TEST_CASE("host export policy") {
    using xs::runtime::detail::host_policy_allows;
    CHECK(host_policy_allows("api-ms-win-core-memory-l1-1-0.dll", "VirtualAlloc"));
    CHECK(host_policy_allows("KERNEL32.dll", "VirtualAlloc"));
    CHECK(host_policy_allows("VCRUNTIME140.dll", "memcpy"));
    CHECK(host_policy_allows("api-ms-win-crt-stdio-l1-1-0.dll", "__acrt_iob_func"));
    CHECK_FALSE(host_policy_allows("kernel32.dll", "WinExec")); // not a Game OS API name
    CHECK_FALSE(host_policy_allows("kernel32.dll", "#1"));      // ordinals are never forwarded
    for (auto module : {"d3d12_xs.dll", "D3D12.dll", "pixevt.dll", "kernelx.dll", "gdi32.dll",
                        "title-local.dll", "C:\\Windows\\System32\\kernel32.dll"})
        CHECK_FALSE(host_policy_allows(module, "VirtualAlloc"));
}

TEST_CASE("bind plan") {
    xs::runtime::Registry registry;
    registry.add("fixture.dll", "#17", reinterpret_cast<void *>(&probe));
    const auto plan = xs::runtime::bind_plan(file("synthetic.pe"), registry);
    REQUIRE(plan.at("imports").size() == 2);
    CHECK(plan["imports"][0]["origin"] == "missing");
    CHECK(plan["imports"][1]["origin"] == "own");
    CHECK(plan.at("own") == 1);
    CHECK(plan.at("missing") == 1);
    const auto delayed = xs::runtime::bind_plan(file("delay.pe"), {});
    CHECK(delayed.at("delay_load_modules").size() == 2);
    CHECK(delayed["imports"][1]["origin"] == "missing"); // WinExec, on every host
    if (!xs::runtime::execution_supported()) {
        CHECK(delayed.at("host") == 0);
        CHECK_THROWS(xs::runtime::execute("synthetic.pe", file("synthetic.pe"), registry, true));
    }
}
