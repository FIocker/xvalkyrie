#include "app/launch.hpp"
#include "compatibility/assessment.hpp"
#include "compatibility/modules.hpp"
#include "formats/pe/inspect.hpp"
#include <catch2/catch.hpp>
#include <fstream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <vector>

namespace {
using json = nlohmann::json;
using xs::compat::classify_module;
std::vector<std::uint8_t> file(const char *name) {
    std::ifstream input(name, std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    REQUIRE_FALSE(bytes.empty());
    return bytes;
}
json profile_of(std::vector<std::string> modules) {
    auto imports = json::array();
    for (auto &module : modules)
        imports.push_back({{"module", std::move(module)}, {"symbols", json::array()}});
    return xs::compat::import_profile({{"directory_contents", {{"imports", std::move(imports)}}}});
}
} // namespace

TEST_CASE("module classification") {
    CHECK(classify_module("D3D12_XS.DLL").category == "d3d_scarlett");
    CHECK(classify_module("xg_xs.dll").category == "d3d_scarlett");
    CHECK(classify_module("d3d12_x.dll").category == "d3d_xbox_one");
    CHECK(classify_module("D3D12.dll").category == "d3d_stock");
    CHECK(classify_module("kernelx.dll").category == "legacy_era");
    CHECK(classify_module("PIXEvt.dll").category == "game_os_only");
    CHECK(classify_module("api-ms-win-core-memory-l1-1-0.dll").os_surface);
    CHECK(classify_module("api-ms-win-crt-runtime-l1-1-0.dll").category == "os_crt");
    CHECK_FALSE(classify_module("ucrtbase.dll").os_surface);
    CHECK(classify_module("VCRUNTIME140_1D.dll").development_only);
    CHECK_FALSE(classify_module("vcruntime140.dll").development_only);
    CHECK(classify_module("xgameruntime.thunks.dll").category == "gdk");
    // must match the whole name, not a substring
    for (auto name : {"d3d12_xs.dll.bak", "notd3d12_xs.dll", "d3d12_xs", "", "api-ms-win-core.dll"})
        CHECK(classify_module(name).category == "unknown");
    CHECK(xs::compat::is_game_os_api("VirtualAlloc"));
    CHECK(xs::compat::is_game_os_api("DStorageGetFactory"));
    CHECK_FALSE(xs::compat::is_game_os_api("virtualalloc"));
    CHECK_FALSE(xs::compat::is_game_os_api("WinExec"));
}

TEST_CASE("series template module list") {
    const auto report = xs::pe::inspect(file("scarlett-modules.pe"));
    CHECK(report.at("subsystem") == 2); // Windows GUI, as in the published tool output
    const auto profile = xs::compat::import_profile(report);
    CHECK(profile.at("target_indication") == "scarlett");
    std::map<std::string, int> categories;
    for (const auto &module : profile.at("modules"))
        ++categories[module.at("category").get<std::string>()];
    // same numbers xbdepends prints in its readme
    CHECK(categories == std::map<std::string, int>{
                            {"api_set", 22}, {"d3d_scarlett", 1}, {"game_os_only", 1}, {"crt", 3}});
    REQUIRE(profile.at("findings").size() == 1);
    CHECK(profile["findings"][0]["finding"] == "development-only module");
    CHECK(profile["findings"][0]["names"].size() == 3);
    const auto assessment = xs::compat::assess(report);
    CHECK(assessment.at("console_target_verified") == false);
    CHECK(assessment.at("execution_supported") == false);
}

TEST_CASE("target detection") {
    CHECK(profile_of({"kernel32.dll", "xgameruntime.dll"})["target_indication"] == "unknown");
    CHECK(profile_of({"d3d12_x.dll"})["target_indication"] == "xbox_one");
    CHECK(profile_of({"dxgi.dll", "d3d9.dll"})["target_indication"] == "pc");
    const auto mixed = profile_of({"d3d12_xs.dll", "D3D12.dll", "kernelx.dll", "gdi32.dll"});
    CHECK(mixed["target_indication"] == "mixed");
    CHECK(mixed["findings"].size() == 3);
}

TEST_CASE("delay imports") {
    auto bytes = file("delay.pe");
    const auto report = xs::pe::inspect(bytes);
    const auto &delayed = report.at("directory_contents").at("delay_imports");
    REQUIRE(delayed.size() == 2);
    CHECK(delayed[0].at("module") == "dxcompiler_xs.dll");
    CHECK(delayed[0].at("symbols")[0].at("ordinal") == 1);
    CHECK(delayed[1].at("symbols")[0].at("name") == "Probe");
    const auto assessment = xs::compat::assess(report);
    const auto &profile = assessment.at("import_profile");
    CHECK(profile.at("target_indication") == "scarlett");
    CHECK(profile.at("modules")[1].at("delay_load") == true);
    CHECK(profile.at("game_os_api_names") == 1);
    CHECK(profile.at("findings").back().at("names") == json::array({"KERNEL32.dll!WinExec"}));
    CHECK(assessment.at("delay_imports")[0].at("symbols")[0].at("implementation") == "unsupported");

    const auto descriptor = 152 + 112 + 8 * 13;
    const auto table = 512 + (bytes[descriptor] | bytes[descriptor + 1] << 8) - 0x1000;
    SECTION("VA-based descriptor") {
        bytes[table] = 0;
        CHECK_THROWS(xs::pe::inspect(bytes));
    }
    SECTION("missing terminator") {
        bytes[descriptor + 4] = 64;
        CHECK_THROWS(xs::pe::inspect(bytes));
    }
}

TEST_CASE("launch series image") {
    const auto scarlett = xs::launch_title("scarlett-modules.pe");
    CHECK(scarlett.status == xs::LaunchStatus::runtime_unavailable);
    CHECK(scarlett.detail.find("d3d12_xs.dll") != std::string::npos);
    CHECK(scarlett.detail.find("No guest code ran") != std::string::npos);
    CHECK(xs::launch_title("delay.pe").status == xs::LaunchStatus::runtime_unavailable);
}
