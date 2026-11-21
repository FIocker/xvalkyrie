#include "formats/pri/resources.hpp"
#include <catch2/catch.hpp>
#include <cstring>
#include <vector>

TEST_CASE("pri references") {
    const xv::pri::Strings strings{
        {"resources/appname", {{"fr-FR", "Jeu"}, {"en-GB", "Game (GB)"}, {"", "Game"}}},
        {"resources/onlyfrench", {{"fr-FR", "Seul"}}},
        {"other/name", {{"", "Other"}}}};
    CHECK(xv::pri::resolve(strings, "ms-resource:AppName", "fr-FR") == "Jeu");
    // the same language of another region, before the one for any language
    CHECK(xv::pri::resolve(strings, "ms-resource:AppName") == "Game (GB)");
    CHECK(xv::pri::resolve(strings, "MS-Resource:appname", "ja-JP") == "Game");
    CHECK(xv::pri::resolve(strings, "ms-resource:OnlyFrench") == "Seul");
    CHECK(xv::pri::resolve(strings, "ms-resource:///Other/Name") == "Other");
    CHECK(xv::pri::resolve(strings, "ms-resource:Other/Name") == "Other");
    CHECK_FALSE(xv::pri::resolve(strings, "ms-resource:Missing"));
    CHECK_FALSE(xv::pri::resolve(strings, "ms-resource:"));
    CHECK_FALSE(xv::pri::resolve(strings, "AppName"));
}

TEST_CASE("pri bad input") {
    std::vector<std::uint8_t> bytes(64);
    CHECK_THROWS(xv::pri::strings(bytes)); // no magic
    std::memcpy(bytes.data(), "mrm_pri2", 8);
    CHECK_THROWS(xv::pri::strings(std::span(bytes).first(16)));
    CHECK(xv::pri::strings(bytes).empty()); // no sections
    // a table of contents that points outside the file
    bytes[16] = 32; // the table
    bytes[24] = 3;  // three sections, of which there is room for one
    CHECK_THROWS(xv::pri::strings(bytes));
    // one resource map whose own tables are not there
    bytes[24] = 1;
    std::memcpy(bytes.data() + 32, "[mrm_res_map2_]", 15);
    bytes[32 + 24] = 200; // where the section is said to start
    CHECK_THROWS(xv::pri::strings(bytes));
}
