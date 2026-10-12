#include "formats/pe/inspect.hpp"
#include <catch2/catch.hpp>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <vector>

namespace {
constexpr std::size_t raw = 512, dirs = 264;
auto fixture() {
    std::ifstream input("synthetic.pe", std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    REQUIRE(bytes.size() == 1024);
    return bytes;
}
void put(std::vector<std::uint8_t> &bytes, std::size_t pos, std::uint64_t value,
         std::size_t width = 4) {
    for (std::size_t i = 0; i < width; ++i)
        bytes.at(pos + i) = static_cast<std::uint8_t>(value >> (8 * i));
}
} // namespace

TEST_CASE("pe directories") {
    const auto contents = xs::pe::inspect(fixture()).at("directory_contents");
    const auto &module = contents.at("imports").at(0);
    CHECK(module.at("module") == "fixture.dll");
    CHECK(module.at("symbols").at(0).at("name") == "Probe");
    CHECK(module.at("symbols").at(0).at("hint") == 7);
    CHECK(module.at("symbols").at(1).at("ordinal") == 17);
    const auto &symbols = contents.at("exports").at("symbols");
    CHECK(symbols.at(0).at("names") == nlohmann::json::array({"Alias", "Value"}));
    CHECK(symbols.at(1).at("ordinal") == 6);
    CHECK(symbols.at(1).at("forwarder") == "fixture.Probe");
    CHECK(contents.at("base_relocations").at(0).at("entries").at(0).at("type") == 10);
    CHECK(contents.at("tls").at("callbacks_va").at(0) == 0x140001180ULL);
    CHECK(contents.at("exceptions").at(0).at("unwind_info_rva") == 0x1190);
}

TEST_CASE("pe directory variations") {
    auto bytes = fixture();
    SECTION("no directories") {
        put(bytes, 152 + 108, 0);
        CHECK(xs::pe::inspect(bytes).at("directory_contents").empty());
    }
    SECTION("no ILT, falls back to IAT") {
        put(bytes, raw, 0);
        CHECK(xs::pe::inspect(bytes)["directory_contents"]["imports"][0]["symbols"].size() == 2);
    }
    SECTION("bound IAT") {
        put(bytes, raw, 0);
        put(bytes, raw + 4, 1);
        CHECK_FALSE(xs::pe::inspect(bytes)["directory_contents"]["imports"][0].contains("symbols"));
    }
    SECTION("name in headers") {
        put(bytes, raw + 12, 64);
        bytes[64] = 'Q';
        CHECK(xs::pe::inspect(bytes)["directory_contents"]["imports"][0]["module"] == "Q");
    }
    SECTION("name in another section") {
        bytes.resize(1536);
        put(bytes, 134, 2, 2);
        const auto second = 392 + 40;
        put(bytes, second + 8, 512);
        put(bytes, second + 12, 0x2000);
        put(bytes, second + 16, 512);
        put(bytes, second + 20, 1024);
        put(bytes, 152 + 56, 0x3000);
        put(bytes, raw + 12, 0x2008);
        bytes[1032] = 'Z';
        CHECK(xs::pe::inspect(bytes)["directory_contents"]["imports"][0]["module"] == "Z");
    }
    SECTION("exceptions on arm64") {
        put(bytes, 132, 0xaa64, 2);
        CHECK(xs::pe::inspect(bytes)["directory_contents"]["exceptions"].contains("status"));
    }
    SECTION("HIGHADJ relocation") {
        put(bytes, raw + 0x108, 0x4001, 2);
        put(bytes, raw + 0x10a, 0xf234, 2);
        const auto entries =
            xs::pe::inspect(bytes)["directory_contents"]["base_relocations"][0]["entries"];
        REQUIRE(entries.size() == 1);
        CHECK(entries.at(0).at("adjustment_word") == 0xf234);
    }
}

TEST_CASE("malformed pe directories") {
    auto bytes = fixture();
    SECTION("lookup table in virtual tail") {
        put(bytes, 392 + 8, 1024);
        put(bytes, raw, 0x1200);
    }
    SECTION("lookup table in padding") {
        put(bytes, 392 + 8, 0x180);
        put(bytes, raw, 0x11c0);
    }
    SECTION("import terminator out of range") { put(bytes, dirs + 8 + 4, 20); }
    SECTION("unterminated module name") {
        put(bytes, raw + 12, 0x11ff);
        bytes.back() = 'x';
    }
    SECTION("unterminated forwarder") { put(bytes, dirs + 4, 0x69); }
    SECTION("export ordinal out of range") { put(bytes, raw + 0xc8, 2, 2); }
    SECTION("huge export count") { put(bytes, raw + 0x80 + 20, 0xffffffff); }
    SECTION("empty relocation block") { put(bytes, raw + 0x104, 0); }
    SECTION("HIGHADJ at block end") { put(bytes, raw + 0x10a, 0x4000, 2); }
    SECTION("TLS callbacks given as RVA") { put(bytes, raw + 0x128, 0x1140, 8); }
    SECTION("unterminated TLS callbacks") {
        put(bytes, raw + 0x128, 0x1400011f8ULL, 8);
        put(bytes, raw + 0x1f8, 0x140001180ULL, 8);
    }
    SECTION("partial exception entry") { put(bytes, dirs + 3 * 8 + 4, 13); }
    SECTION("section overlaps headers") { put(bytes, 392 + 12, 0x180); }
    CHECK_THROWS_AS(xs::pe::inspect(bytes), std::runtime_error);
}
