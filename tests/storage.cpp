#include "runtime/durango/storage.hpp"
#include <catch2/catch.hpp>
#include <chrono>
#include <filesystem>

namespace {
namespace fs = std::filesystem;
using xs::runtime::durango::SaveSpace;
struct Folder {
    fs::path path = fs::temp_directory_path() /
                    ("xscarlet-saves-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Folder() { fs::remove_all(path); }
};
std::vector<std::uint8_t> bytes(std::string_view text) { return {text.begin(), text.end()}; }
} // namespace

TEST_CASE("save names become portable file names and back") {
    using xs::runtime::durango::escape_save_name;
    using xs::runtime::durango::unescape_save_name;
    for (const std::string name : {"slot1", "Slot 1", "a/b\\c:d", ".hidden.", "con", "COM1.txt",
                                   "\xc3\xb1"
                                   "and\xc3\xba",
                                   "Save^%#"}) {
        const auto file = escape_save_name(name);
        CAPTURE(name, file);
        CHECK(file.find_first_of("/\\: ") == std::string::npos);
        CHECK(file.front() != '.');
        CHECK(file.back() != '.');
        CHECK(unescape_save_name(file) == name);
    }
    CHECK(escape_save_name("Save") != escape_save_name("save"));
    CHECK_FALSE(unescape_save_name("#display"));
}

TEST_CASE("a save space keeps containers and blobs in files") {
    Folder folder;
    {
        SaveSpace space(folder.path);
        CHECK_FALSE(space.read("profile", "data"));
        CHECK(space.containers().empty());
        space.submit("profile", {{"data", bytes("hello")}, {"Extra", bytes("x")}}, {}, "My save");
        space.submit("profile 2", {{"data", bytes("other")}}, {});
    }
    // another instance, as in the next run of the title
    SaveSpace space(folder.path);
    CHECK(space.read("profile", "data") == bytes("hello"));
    CHECK(space.read("profile", "Extra") == bytes("x"));
    CHECK_FALSE(space.read("profile", "extra"));

    const auto all = space.containers();
    REQUIRE(all.size() == 2);
    CHECK(all[0].name == "profile");
    CHECK(all[0].display_name == "My save");
    CHECK(all[0].size == 6);
    CHECK(space.containers("profile ").size() == 1);

    space.submit("profile", {{"data", bytes("bye")}}, {"Extra", "missing"});
    CHECK(space.read("profile", "data") == bytes("bye"));
    CHECK_FALSE(space.read("profile", "Extra"));
    const auto blobs = space.blobs("profile");
    REQUIRE(blobs.size() == 1);
    CHECK(blobs[0].name == "data");
    CHECK(blobs[0].size == 3);
    CHECK(space.containers("profile")[0].display_name == "My save");

    CHECK(space.remove("profile"));
    CHECK_FALSE(space.remove("profile"));
    CHECK(space.containers().size() == 1);
}
