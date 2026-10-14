#include "app/launch.hpp"
#include "formats/appx/manifest.hpp"
#include <catch2/catch.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {
namespace fs = std::filesystem;
nlohmann::json manifest(std::string_view text) {
    return xs::appx::inspect_manifest(
        {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}
std::string package(std::string_view executable) {
    return "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           "<Package xmlns:mx=\"http://schemas.microsoft.com/appx/2013/xbox/manifest\">"
           "<Identity Name=\"Sample\" Version=\"1.2.3.4\" ProcessorArchitecture=\"x64\"/>"
           "<Properties><DisplayName>Sample Title</DisplayName></Properties>"
           "<Prerequisites><mx:OSName>era</mx:OSName></Prerequisites>"
           "<Applications><Application Id=\"App\" Executable=\"" +
           std::string(executable) + "\" EntryPoint=\"Sample.App\"/></Applications></Package>";
}
// title directory laid out like an unpacked package: <root>/Mount/{manifest, executable}
fs::path title_directory(const char *name, std::string_view executable) {
    const auto root = fs::absolute(name);
    fs::remove_all(root);
    fs::create_directories(root / "Mount");
    std::ofstream(root / "Mount" / "AppxManifest.xml", std::ios::binary) << package(executable);
    fs::copy_file("durango-modules.pe", root / "Mount" / "game.exe");
    return root;
}
} // namespace

TEST_CASE("package manifest") {
    const auto report = manifest(package("game.exe"));
    CHECK(report.at("name") == "Sample");
    CHECK(report.at("version") == "1.2.3.4");
    CHECK(report.at("display_name") == "Sample Title");
    CHECK(report.at("os_name") == "era");
    CHECK(report.at("environment").is_null());
    REQUIRE(report.at("applications").size() == 1);
    CHECK(report["applications"][0].at("executable") == "game.exe");
    CHECK(report["applications"][0].at("entry_point") == "Sample.App");
    for (auto xml : {"", "<Package>", "<Other/>", "<Package/>",
                     "<Package><Identity/><Applications/></Package>",
                     "<Package><Identity/><Applications><Application/></Applications></Package>",
                     "<!DOCTYPE Package><Package/>"})
        CHECK_THROWS(manifest(xml));
}

TEST_CASE("title directory") {
    const auto root = title_directory("title-directory", "game.exe");
    const auto executable = root / "Mount" / "game.exe";
    CHECK(xs::title_executable(root) == executable);
    CHECK(xs::title_executable(root / "Mount") == executable);
    CHECK(xs::title_executable(root / "Mount" / "AppxManifest.xml") == executable);
    CHECK(xs::title_executable(executable) == executable);
    const auto result = xs::launch_title(root);
    CHECK(result.status == xs::LaunchStatus::runtime_unavailable);
    CHECK(result.target == xs::Target::durango);
    CHECK(result.detail.starts_with("Sample Title 1.2.3.4, executable game.exe. "));
    CHECK(result.detail.find("d3d12_x.dll") != std::string::npos);

    fs::remove(root / "Mount" / "AppxManifest.xml");
    CHECK_THROWS(xs::title_executable(root));
    CHECK(xs::launch_title(root).status == xs::LaunchStatus::unreadable);
    // the manifest must name a file next to it
    for (auto name : {"..\\game.exe", "../game.exe", "C:game.exe", "sub/game.exe", ".."}) {
        const auto escaping = title_directory("title-directory", name);
        CHECK_THROWS(xs::title_executable(escaping));
    }
    fs::remove_all(root);
}
