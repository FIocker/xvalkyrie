#include "graphics/abstraction/clear_pass.hpp"
#include "graphics/backend.hpp"
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

TEST_CASE("graphics backends") {
    using namespace xs::graphics;
    CHECK(parse_backend("d3d11") == Backend::d3d11);
    CHECK(parse_backend("vulkan") == Backend::vulkan);
    CHECK_FALSE(parse_backend("D3D11"));
    CHECK_FALSE(parse_backend(""));
    CHECK(backend_name(Backend::vulkan) == "vulkan");
    // Vulkan exists everywhere and renders no title; Direct3D 11 is the host's own
    CHECK(backend_available(Backend::vulkan));
    CHECK_FALSE(backend_renders_titles(Backend::vulkan));
    CHECK(backend_available(Backend::d3d11) == backend_renders_titles(Backend::d3d11));
    CHECK(backend_available(default_backend()));
    CHECK(selected_backend() == default_backend());
    if (backend_available(Backend::d3d11)) {
        CHECK(default_backend() == Backend::d3d11);
        select_backend(Backend::vulkan);
        CHECK(selected_backend() == Backend::vulkan);
        select_backend(Backend::d3d11);
    } else {
        CHECK(default_backend() == Backend::vulkan);
        CHECK_THROWS(select_backend(Backend::d3d11));
    }
}
