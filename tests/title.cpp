#include "formats/appx/manifest.hpp"
#include "launcher/launch.hpp"
#include "launcher/library.hpp"
#include <algorithm>
#include <catch2/catch.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

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
           "<Properties><DisplayName>Sample Title</DisplayName>"
           "<Logo>Assets/logo.png</Logo></Properties>"
           "<Prerequisites><mx:OSName>era</mx:OSName></Prerequisites>"
           "<Applications><Application Id=\"App\" Executable=\"" +
           std::string(executable) +
           "\" EntryPoint=\"Sample.App\"><VisualElements>"
           "<SplashScreen Image=\"Assets\\splash.png\"/></VisualElements></Application>"
           "</Applications></Package>";
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
    CHECK(report.at("logo") == "Assets/logo.png");
    CHECK(report.at("splash") == "Assets\\splash.png");
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

TEST_CASE("library discovery") {
    const auto root = fs::temp_directory_path() /
                      ("xscarlet-library-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(root));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    const auto game = root / "sample";
    fs::create_directories(game / "Mount" / "Assets");
    std::ofstream(game / "Mount" / "AppxManifest.xml") << package("game.exe");
    fs::copy_file("durango-modules.pe", game / "Mount" / "game.exe");
    std::ofstream(game / "Mount" / "Assets" / "logo.scale-200.png") << "logo";
    std::ofstream(game / "Mount" / "Assets" / "splash.png") << "splash";

    const auto found = xs::discover_title(game);
    REQUIRE(found);
    CHECK(found->name == "Sample Title");
    CHECK(found->identity == "Sample");
    CHECK(found->version == "1.2.3.4");
    CHECK(found->target == xs::Target::durango);
    CHECK(found->executable == game / "Mount" / "game.exe");
    CHECK(found->logo == game / "Mount" / "Assets" / "logo.scale-200.png");
    CHECK(found->splash == game / "Mount" / "Assets" / "splash.png");
    CHECK(xs::title_splash(game) == found->splash);
    CHECK(xs::title_splash(found->executable) == found->splash);
    CHECK(xs::title_splash(root / "missing").empty());

    const auto flat = root / "flat";
    fs::create_directory(flat);
    std::ofstream(flat / "AppxManifest.xml") << package("app.exe");
    fs::copy_file("scarlett-modules.pe", flat / "app.exe");
    REQUIRE(xs::discover_title(flat));
    CHECK(xs::discover_title(flat)->target == xs::Target::scarlett);
    const auto unsafe = root / "unsafe";
    fs::create_directory(unsafe);
    std::ofstream(unsafe / "AppxManifest.xml") << package("../app.exe");
    CHECK_FALSE(xs::discover_title(unsafe));

    auto bytes = std::vector<char>(0x1000);
    std::memcpy(bytes.data() + 0x200, "msft-xvd", 8);
    bytes[0x284] = 1; // title
    bytes[0x471] = 4; // tentative Series platform mask
    const auto xvc = root / "package.xvc";
    std::ofstream(xvc, std::ios::binary).write(bytes.data(), bytes.size());
    std::ofstream(root / "broken.xvd") << "not an XVD";
    const auto titles = xs::discover_library({root, game});
    REQUIRE(titles.size() == 3);
    const auto package_title =
        std::find_if(titles.begin(), titles.end(), [](const auto &title) { return title.package; });
    REQUIRE(package_title != titles.end());
    CHECK(package_title->path == xvc);
    CHECK(package_title->target == xs::Target::scarlett);
    CHECK(std::count_if(titles.begin(), titles.end(), [&](const auto &title) {
              return title.path == game && !title.package;
          }) == 1);

    // the cache: the same titles, without reading the unchanged ones again
    const auto cache = root / "cache" / "library.json";
    CHECK(xs::cached_library({root}, cache).empty());
    REQUIRE(xs::discover_library({root}, cache).size() == 3);
    const auto cached = xs::cached_library({root}, cache);
    REQUIRE(cached.size() == 3);
    CHECK(xs::cached_library({root / "elsewhere"}, cache).empty());
    const auto sample = std::find_if(cached.begin(), cached.end(),
                                     [&](const auto &title) { return title.path == game; });
    REQUIRE(sample != cached.end());
    CHECK(sample->name == "Sample Title");
    CHECK(sample->target == xs::Target::durango);
    CHECK(sample->executable == found->executable);
    CHECK(sample->logo == found->logo);

    nlohmann::json json;
    std::ifstream(cache) >> json;
    for (auto &entry : json.at("titles"))
        entry["name"] = "From cache";
    std::ofstream(cache, std::ios::trunc) << json.dump();
    const auto again = xs::discover_library({root}, cache);
    CHECK(std::count_if(again.begin(), again.end(),
                        [](const auto &title) { return title.name == "From cache"; }) == 3);
    // a title whose manifest changed is read again
    std::ofstream(game / "Mount" / "AppxManifest.xml", std::ios::trunc)
        << package("game.exe") << "\n";
    const auto changed = xs::discover_library({root}, cache);
    CHECK(std::count_if(changed.begin(), changed.end(),
                        [](const auto &title) { return title.name == "Sample Title"; }) == 1);
    CHECK(xs::title_save_directory(game).filename() == "Sample");
    CHECK(xs::title_save_directory(game).parent_path().filename() == "saves");
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
