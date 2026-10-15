#include "app/launch.hpp"
#include "compatibility/assessment.hpp"
#include "formats/gdk/game_config.hpp"
#include <catch2/catch.hpp>
#include <nlohmann/json.hpp>

namespace {
using json = nlohmann::json;
using xs::compat::assess;
using xs::compat::compare_declaration;
json config(std::string_view text) {
    return xs::gdk::inspect_config(
        {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}
json pe() {
    return {{"architecture", "x86_64"},
            {"machine", 0x8664},
            {"format", "PE32+"},
            {"platform_indicator", "Xbox subsystem; generation unknown"},
            {"directory_contents", json::object()}};
}
TEST_CASE("assess imports") {
    auto input = pe();
    input["directory_contents"]["imports"] = json::array(
        {{{"module", "Kernel32.DLL"},
          {"symbols",
           json::array(
               {{{"name", "VirtualAlloc"}}, {{"name", "virtualalloc"}}, {{"ordinal", 7}}})}},
         {{"module", "unrelated.dll"},
          {"symbols",
           json::array({{{"name", "VirtualAlloc"}}, {{"name", "XGameRuntimeInitialize"}}})}},
         {{"module", "bound.dll"}, {"status", "bound IAT without lookup table; symbols unknown"}}});
    const auto report = assess(input);
    const auto &modules = report.at("standard_imports");
    CHECK(modules[0]["category"] == "win32_legacy");
    CHECK(modules[0]["symbols"][0]["evidence_match"] == "game_os_api_name");
    CHECK(modules[0]["symbols"][0]["implementation"] == "unsupported");
    CHECK(modules[0]["symbols"][1]["evidence_match"] == "unknown");
    CHECK(modules[0]["symbols"][2]["ordinal"] == 7);
    CHECK(modules[0]["symbols"][2]["evidence_match"] == "unknown");
    CHECK(modules[1]["symbols"][0]["evidence_match"] == "unknown");
    CHECK(modules[1]["symbols"][1]["evidence_match"] == "source_name_only");
    CHECK(modules[2]["symbols"].is_null());
    CHECK(report["target"] == "unknown");
    CHECK(report["console_target_verified"] == false);
    CHECK(report["execution_supported"] == false);
}

TEST_CASE("assess config") {
    const auto decl = config(R"(<Game><ExecutableList>
        <Executable Name="bin\A.exe" TargetDeviceFamily="Scarlett" Architecture="x64"/>
        <Executable Name="B.exe"/><Executable Name="C.exe" TargetDeviceFamily="Future"/>
        <Executable Name="D.exe" TargetDeviceFamily="XboxOne"/>
        </ExecutableList></Game>)");
    const auto built_for = [](const char *module) {
        auto image = pe();
        image["directory_contents"]["imports"] =
            json::array({{{"module", module}, {"symbols", json::array()}}});
        return image;
    };
    const auto input = built_for("d3d12_xs.dll");
    CHECK(assess(input)["target"] == "scarlett");
    const auto match = compare_declaration(input, decl, R"(bin\A.exe)");
    CHECK(match["family_vs_target"] == "consistent");
    CHECK(match["declared_architecture_vs_pe"] == "consistent");
    CHECK(compare_declaration(input, decl, "D.exe")["family_vs_target"] == "conflict");
    const auto durango = built_for("d3d12_x.dll");
    CHECK(assess(durango)["target"] == "durango");
    CHECK(compare_declaration(durango, decl, "D.exe")["family_vs_target"] == "consistent");
    CHECK(compare_declaration(durango, decl, R"(bin\A.exe)")["family_vs_target"] == "conflict");
    CHECK(compare_declaration(pe(), decl, "D.exe")["family_vs_target"] == "unknown");
    auto arm = input;
    arm["machine"] = 0xaa64;
    arm["architecture"] = "arm64";
    CHECK(assess(arm)["within_x64_scope"] == false);
    CHECK(compare_declaration(arm, decl, R"(bin\A.exe)")["declared_architecture_vs_pe"] ==
          "conflict");
    CHECK(compare_declaration(input, decl, "B.exe")["family_vs_target"] == "unknown");
    CHECK(decl["executables"][1]["declared_architecture"].is_null());
    CHECK(decl["executables"][2]["device_family_status"] == "unknown_value");
    CHECK_THROWS(compare_declaration(input, decl, "A.exe"));
    const auto ambiguous = config(R"(<Game><ExecutableList><Executable Name="A"/>
        <Executable Name="A"/></ExecutableList></Game>)");
    CHECK_THROWS(compare_declaration(input, ambiguous, "A"));
}

TEST_CASE("launch status") {
    CHECK(xs::launch_title("host-input.bin").status == xs::LaunchStatus::invalid_image);
    CHECK(xs::launch_title("absent-title.exe").status == xs::LaunchStatus::unreadable);
}

TEST_CASE("bad game config") {
    for (auto xml :
         {"<Game>", "<Other/>", "<Game><ExecutableList/></Game>",
          "<Game><ExecutableList><Executable/></ExecutableList></Game>", "<!DOCTYPE Game><Game/>",
          "<Game><ExecutableList><Executable Name='a' Name='b'/></ExecutableList></Game>",
          "<Game xmlns='other'><ExecutableList><Executable Name='a'/></ExecutableList></Game>"})
        CHECK_THROWS(config(xml));
    CHECK_THROWS(config(std::string(xs::gdk::config_limit + 1, ' ')));
}
} // namespace
