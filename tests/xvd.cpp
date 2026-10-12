#include "app/launch.hpp"
#include "formats/xvd/identify.hpp"
#include <catch2/catch.hpp>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

namespace {
// fake header built by hand, see docs/research/packages.md for the layout
std::vector<std::uint8_t> header() {
    std::vector<std::uint8_t> bytes(xs::xvd::header_size);
    std::memcpy(bytes.data() + 0x200, "msft-xvd", 8);
    bytes[0x208] = 0x41;     // ReadOnly | RegionIdInXts
    bytes[0x20c] = 3;        // format version
    bytes[0x21a] = 0x10;     // drive size 0x100000
    bytes[0x220] = 0xab;     // first VDUID byte
    bytes[0x284] = 1;        // Title
    bytes[0x298 + 1] = 0xa0; // block size 0xaa000
    bytes[0x298 + 2] = 0x0a;
    std::memcpy(bytes.data() + 0x38c, "XDKS.1", 6);
    bytes[0x3bc] = 7;
    bytes[0x471] = 4;
    return bytes;
}
} // namespace

TEST_CASE("xvd header fields") {
    auto bytes = header();
    const auto report = xs::xvd::identify(bytes, 1ull << 36);
    CHECK(report.at("file_size") == 1ull << 36);
    CHECK(report.at("format_version") == 3);
    CHECK(report.at("volume_flag_names") == nlohmann::json::array({"ReadOnly", "RegionIdInXts"}));
    CHECK(report.at("drive_size") == 0x100000);
    CHECK(report.at("vduid_bytes") == "ab000000000000000000000000000000");
    CHECK(report.at("content_type_name") == "Title");
    CHECK(report.at("xvc_content_type") == true);
    CHECK(report.at("block_size") == 0xaa000);
    CHECK(report.at("sandbox_id") == "XDKS.1");
    CHECK(report.at("package_version_words")[0] == 7);
    CHECK(report.at("platform_mask").at("gen9") == true);
    CHECK(report.at("platform_mask").at("gen8") == false);
    CHECK(report.at("signature_area") == "empty");
    CHECK(report.at("encryption") == "data regions declared encrypted; no decryption support");
    CHECK(report.at("scarlett_layout_verified") == false);
    CHECK(report.at("mount_supported") == false);

    bytes[0x208] = 0x06;
    bytes[0x209] = 0x01; // a flag bit without a cross-checked name
    bytes[0x284] = 0x30;
    bytes[0x100] = 1;
    const auto other = xs::xvd::identify(bytes, bytes.size());
    CHECK(other.at("encryption") == "disabled by volume flag; contents still not read");
    CHECK(other.at("data_integrity") == "disabled by volume flag");
    CHECK(other.at("unnamed_volume_flag_bits") == 0x100);
    CHECK(other.at("content_type_name") == "unknown");
    CHECK(other.at("xvc_content_type") == false);
    CHECK(other.at("signature_area") == "present; not verified");
}

TEST_CASE("xvd bad input") {
    auto bytes = header();
    CHECK_FALSE(xs::xvd::has_magic(std::span(bytes).first(0x207)));
    CHECK_THROWS(xs::xvd::identify(std::span(bytes).first(0xfff), 0xfff));
    bytes[0x200] = 'M';
    CHECK_FALSE(xs::xvd::has_magic(bytes));
    CHECK_THROWS(xs::xvd::identify(bytes, bytes.size()));
}

TEST_CASE("launch xvd package") {
    const auto bytes = header();
    std::ofstream("synthetic-header.xvd", std::ios::binary)
        .write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    const auto result = xs::launch_title("synthetic-header.xvd");
    CHECK(result.status == xs::LaunchStatus::package_unsupported);
    CHECK(result.detail.find("Title") != std::string::npos);
    CHECK(result.detail.find("decrypted") != std::string::npos);
}
