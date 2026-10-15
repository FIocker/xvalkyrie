#include "graphics/abstraction/clear_pass.hpp"
#include <catch2/catch.hpp>
#include <limits>
#include <stdexcept>

TEST_CASE("clear pass validation") {
    using namespace xs::graphics;
    ClearPass pass{31, 9, {}, {}};
    CHECK(pass.validated_pixel_count() == 279);
    pass.rectangles.push_back({30, 8, 1, 1, {}});
    CHECK(pass.validated_pixel_count() == 279);
    SECTION("width overflow") {
        pass.rectangles[0].width = std::numeric_limits<std::uint32_t>::max();
    }
    SECTION("origin outside the attachment") { pass.rectangles[0].y = 10; }
    SECTION("empty rectangle") { pass.rectangles[0].height = 0; }
    SECTION("oversized allocation") { pass.width = 2049; }
    SECTION("empty attachment") { pass.height = 0; }
    SECTION("too many rectangles") { pass.rectangles.resize(1025, pass.rectangles.front()); }
    CHECK_THROWS_AS(pass.validated_pixel_count(), std::invalid_argument);
}
