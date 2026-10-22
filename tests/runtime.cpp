#include "runtime/console.hpp"
#include "runtime/modules/xg.hpp"
#include <catch2/catch.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
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

TEST_CASE("console registries") {
    const auto durango = xs::runtime::console_registry(xs::Target::durango);
    const auto scarlett = xs::runtime::console_registry(xs::Target::scarlett);
    CHECK(durango.forwarded("KernelX.dll") == "kernel32.dll");
    CHECK(durango.forwarded("kernel32.dll").empty());
    CHECK(durango.find("EtwPlus.dll", "EtxRegister") != nullptr);
    CHECK(scarlett.forwarded("kernelx.dll").empty());
    CHECK(scarlett.find("etwplus.dll", "EtxRegister") == nullptr);
    // no Direct3D yet on either
    CHECK(durango.find("d3d12_x.dll", "D3D12XboxCreateDevice") == nullptr);
    CHECK(scarlett.find("d3d12_xs.dll", "D3D12XboxCreateDevice") == nullptr);
    CHECK(xs::runtime::title_registry(file("durango-modules.pe")).forwarded("kernelx.dll") ==
          "kernel32.dll");
    CHECK(xs::runtime::title_registry(file("synthetic.pe")).forwarded("kernelx.dll").empty());
    CHECK_THROWS(xs::runtime::title_registry({}));
}

TEST_CASE("forwarded modules") {
    using xs::runtime::detail::resolve;
    xs::runtime::Registry registry;
    registry.forward("Era.dll", "KERNEL32.dll");
    CHECK_THROWS(registry.forward("era.dll", "other.dll"));
    CHECK_THROWS(registry.forward("", "kernel32.dll"));
    registry.add("era.dll", "Own", reinterpret_cast<void *>(&probe));
    CHECK(resolve(registry, "era.dll", "Own").origin == std::string_view("own"));
    // not forwarded and not a system DLL
    CHECK(resolve(registry, "other.dll", "VirtualAlloc").function == nullptr);
    const auto era = xs::runtime::bind_plan(file("era-modules.pe"),
                                            xs::runtime::console_registry(xs::Target::durango));
    REQUIRE(era.at("imports").size() == 6);
    CHECK(era["imports"][5]["origin"] == "missing"); // d3d11_x.dll!D3D11XCreateDeviceX
    if (xs::runtime::execution_supported()) {
        CHECK(resolve(registry, "era.dll", "VirtualAlloc").origin == std::string_view("host"));
        CHECK(resolve(registry, "era.dll", "NoSuchFunction").function == nullptr);
        // must not see xscarlet.exe as the main module
        CHECK(resolve(registry, "era.dll", "GetModuleHandleW").origin == std::string_view("own"));
        CHECK(era["imports"][0]["origin"] == "own");  // kernelx.dll!VirtualAlloc: console flags
        CHECK(era["imports"][1]["origin"] == "own");  // kernelx.dll!XMemAllocDefault
        CHECK(era["imports"][2]["origin"] == "own");  // etwplus.dll!EtxRegister
        CHECK(era["imports"][3]["origin"] == "host"); // ws2_32.dll!#1
        CHECK(era["imports"][4]["origin"] == "own");  // d3d11_x.dll!D3D11CreateDevice
        CHECK(era.at("missing") == 1);
    } else {
        CHECK(era.at("missing") == 5); // only EtwPlus needs nothing from the host
    }
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
        CHECK_THROWS(xs::runtime::execute("synthetic.pe", file("synthetic.pe"), registry,
                                          {.allow_missing = true, .trace = false}));
    }
}

TEST_CASE("texture layout") {
    using xs::runtime::modules::format_size;
    using xs::runtime::modules::texture_bytes;
    CHECK(format_size(28).bytes == 4); // R8G8B8A8_UNORM
    CHECK(format_size(10).bytes == 8); // R16G16B16A16_FLOAT
    CHECK(format_size(45).bytes == 4); // D24_UNORM_S8_UINT
    CHECK(format_size(71).blocks);     // BC1_UNORM
    CHECK(format_size(71).bytes == 8);
    CHECK(format_size(77).bytes == 16); // BC3_UNORM
    CHECK(texture_bytes(1920, 1080, 1, 1, 28) == 1920 * 1080 * 4);
    CHECK(texture_bytes(1, 1, 1, 1, 28) == 4096); // whole pages
    CHECK(texture_bytes(64, 64, 1, 6, 28) == 64 * 64 * 4 * 6);
    // 4x4, 2x2, 1x1 blocks of 8 bytes
    CHECK(texture_bytes(16, 16, 3, 1, 71) == 4096);
    // full chain of 256x256: 256^2 * 4 * (1 + 1/4 + ... + 1/65536), in whole pages
    CHECK(texture_bytes(256, 256, 0, 1, 28) == texture_bytes(256, 256, 9, 1, 28));
    CHECK(texture_bytes(256, 256, 9, 1, 28) == 352256);

    // the xg_x.dll exports, called the way a title does
    const auto registry = xs::runtime::console_registry(xs::Target::durango);
    using Create = std::int32_t(XS_GUEST_ABI *)(const std::uint32_t *, void ***);
    const auto create =
        reinterpret_cast<Create>(registry.find("xg_x.dll", "XGCreateTexture2DComputer"));
    REQUIRE(create);
    const std::uint32_t description[15] = {1920, 1080, 1, 1, 28, 1, 0xffffffff, 0, 0x28};
    void **computer = nullptr;
    REQUIRE(create(description, &computer) == 0);
    const auto *const methods = static_cast<void **>(*computer);
    std::vector<std::uint64_t> layout(0xd40 / 8, ~std::uint64_t{0});
    using Layout = std::int32_t(XS_GUEST_ABI *)(void *, void *);
    CHECK(reinterpret_cast<Layout>(methods[2])(computer, layout.data()) == 0);
    CHECK(layout[0] == 1920 * 1080 * 4);
    CHECK(layout[1] == 4096);
    CHECK(layout.back() == 0);
    using Release = std::uint32_t(XS_GUEST_ABI *)(void *);
    CHECK(reinterpret_cast<Release>(methods[1])(computer) == 0);
    using Mode =
        std::uint32_t(XS_GUEST_ABI *)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                      std::uint32_t, std::uint32_t, std::uint32_t);
    const auto mode = reinterpret_cast<Mode>(registry.find("xg_x.dll", "XGComputeOptimalTileMode"));
    REQUIRE(mode);
    CHECK(mode(3, 28, 1920, 1080, 1, 1, 0x20) == 0);
}

TEST_CASE("title DLLs in the bind plan") {
    namespace fs = std::filesystem;
    const auto directory = fs::absolute("title-dlls");
    fs::remove_all(directory);
    fs::create_directories(directory);
    fs::copy_file("library.pe", directory / "library.dll");
    const auto user = file("library-user.pe");
    const auto origins = [&](const xs::runtime::Registry &registry, const fs::path &where) {
        std::vector<std::string> result;
        const auto plan = xs::runtime::bind_plan(user, registry, where);
        for (const auto &import : plan.at("imports"))
            result.push_back(import.at("origin"));
        return result;
    };
    using Origins = std::vector<std::string>;
    CHECK(origins({}, directory) == Origins{"title", "title", "missing", "title"});
    CHECK(origins({}, {}) == Origins(4, "missing"));
    CHECK(origins({}, fs::absolute("no-such-directory")) == Origins(4, "missing"));
    // ours and forwarded modules win over a DLL of the title
    xs::runtime::Registry own;
    own.add("library.dll", "Alias", reinterpret_cast<void *>(&probe));
    CHECK(origins(own, directory) == Origins{"own", "title", "missing", "title"});
    xs::runtime::Registry forwarded;
    forwarded.forward("library.dll", "kernel32.dll");
    CHECK(origins(forwarded, directory)[3] != "title");
    CHECK(xs::runtime::bind_plan(user, {}, directory).at("title") == 3);
    // a file that is not a DLL provides nothing
    fs::copy_file("synthetic.pe", directory / "library.dll", fs::copy_options::overwrite_existing);
    CHECK(origins({}, directory) == Origins(4, "missing"));
    fs::remove_all(directory);
}
