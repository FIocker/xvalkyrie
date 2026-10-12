#define CATCH_CONFIG_MAIN
#include "core/console.hpp"
#include "core/settings.hpp"
#include "formats/pe/inspect.hpp"
#include <catch2/catch.hpp>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

TEST_CASE("pe headers") {
    std::ifstream f("synthetic.pe", std::ios::binary);
    std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(f), {}};
    REQUIRE(data.size() == 1024);
    SECTION("basic fields") {
        const auto result = xs::pe::inspect(data);
        CHECK(result.at("image_base") == 0x140000000ull);
        CHECK(result.at("architecture") == "x86_64");
        CHECK(result.at("sections")[0].at("name") == ".rdata");
        CHECK(result.at("directory_descriptors")[4].contains("file_offset"));
        CHECK(result.at("console_target_verified") == false);
        data[152 + 68] = 14;
        CHECK(xs::pe::inspect(data).at("platform_indicator") ==
              "Xbox subsystem; generation unknown");
    }
    SECTION("truncated file") {
        for (std::size_t n : {0, 63, 151, 391, 431, 511, 1023})
            CHECK_THROWS(xs::pe::inspect(std::span(data).first(n)));
    }
    SECTION("too many directories") {
        data[152 + 108] = 17;
        CHECK_THROWS(xs::pe::inspect(data));
    }
    SECTION("section offset out of range") {
        data[392 + 23] = 0xff;
        CHECK_THROWS(xs::pe::inspect(data));
    }
}
TEST_CASE("settings") {
    std::istringstream partial(R"({"vsync":false})");
    const auto settings = xs::read_settings(partial);
    CHECK_FALSE(settings.vsync);
    std::istringstream defaults("{}");
    CHECK(xs::read_settings(defaults).vsync);
    for (auto invalid : {"[]", R"({"vsync":"false"})", R"({"typo":true})", "{"}) {
        std::istringstream input(invalid);
        CHECK_THROWS(xs::read_settings(input));
    }
}
TEST_CASE("console log") {
    xs::Console a, b;
    a.log().warn("probe={}", 42);
    REQUIRE(a.lines().size() == 1);
    CHECK(a.lines()[0].find("probe=42") != std::string::npos);
    CHECK(b.lines().empty());
}
