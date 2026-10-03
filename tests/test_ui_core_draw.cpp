#include "ftk/ui/core/context.hpp"
#include "ftk/ui/core/draw_list.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>

#include <vector>

namespace core = ftk::ui::core;
using ftk::ui::Color;
using ftk::ui::RoundCorners;

namespace {

constexpr Color RED{1.0f, 0.0f, 0.0f, 1.0f};
constexpr Color BLUE{0.0f, 0.0f, 1.0f, 1.0f};

// A frame of one 200 by 100 root, drawn by `draw`.
const core::DrawData& drawn(core::Context& ui, auto draw) {
    ui.begin_frame(0.0);
    ui.begin_root(1, {200.0f, 100.0f}, 1.0f);
    draw(ui.draw_list());
    ui.end_root();
    return *ui.end_frame().roots.at(0).draw;
}

} // namespace

TEST_CASE("Colours are packed a byte each, red lowest", "[ui-core]") {
    CHECK(core::pack_colour(RED) == 0xFF0000FFu);
    CHECK(core::pack_colour({0.0f, 1.0f, 0.0f, 0.5f}) == 0x8000FF00u);
    CHECK(core::pack_colour({2.0f, -1.0f, 0.0f, 1.0f}) == 0xFF0000FFu);
}

TEST_CASE("Rectangles are one shape each, rounded by the corners asked for", "[ui-core]") {
    core::Context ui;
    const core::DrawData& data = drawn(ui, [](core::DrawList& dl) {
        dl.rect_filled({10.0f, 10.0f}, {50.0f, 30.0f}, RED, 4.0f);
        dl.rect({10.0f, 40.0f}, {50.0f, 60.0f}, BLUE, 4.0f, RoundCorners::Top, 2.0f);
        // Rounded past half its height: no more than a pill.
        dl.rect_filled({60.0f, 10.0f}, {100.0f, 20.0f}, RED, 99.0f);
        dl.rect_gradient({0.0f, 70.0f}, {200.0f, 80.0f}, RED, BLUE);
    });
    REQUIRE(data.primitives.size() == 4);
    const core::Primitive& filled = data.primitives[0];
    CHECK(filled.kind == core::PrimitiveKind::Rect);
    CHECK(filled.min == glm::vec2{10.0f, 10.0f});
    CHECK(filled.max == glm::vec2{50.0f, 30.0f});
    CHECK(filled.shape == glm::vec4{4.0f});
    CHECK(filled.thickness == 0.0f);
    CHECK(filled.colour == core::pack_colour(RED));
    CHECK(filled.clip == 0);

    const core::Primitive& outlined = data.primitives[1];
    CHECK(outlined.shape == glm::vec4{4.0f, 4.0f, 0.0f, 0.0f});
    CHECK(outlined.thickness == 2.0f);
    CHECK(data.primitives[2].shape == glm::vec4{5.0f});
    CHECK(data.primitives[3].colour == core::pack_colour(RED));
    CHECK(data.primitives[3].colour2 == core::pack_colour(BLUE));
}

TEST_CASE("Lines, circles, arcs, triangles and quads become the shapes they are", "[ui-core]") {
    core::Context ui;
    const core::DrawData& data = drawn(ui, [](core::DrawList& dl) {
        dl.line({10.0f, 10.0f}, {30.0f, 20.0f}, RED, 2.0f);
        dl.circle_filled({100.0f, 50.0f}, 10.0f, RED);
        dl.circle({100.0f, 50.0f}, 10.0f, RED, 2.0f);
        dl.arc({100.0f, 50.0f}, 20.0f, 0.0f, 1.5f, BLUE, 3.0f);
        dl.triangle_filled({0.0f, 0.0f}, {10.0f, 0.0f}, {5.0f, 8.0f}, RED);
        dl.quad_filled({0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}, {0.0f, 10.0f}, RED);
        dl.quad({0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}, {0.0f, 10.0f}, RED);
        const std::vector<glm::vec2> zigzag{{0.0f, 0.0f}, {10.0f, 10.0f}, {20.0f, 0.0f}};
        dl.polyline(zigzag, BLUE);
    });
    REQUIRE(data.primitives.size() == 1 + 1 + 1 + 1 + 1 + 2 + 4 + 2);
    const core::Primitive& line = data.primitives[0];
    CHECK(line.kind == core::PrimitiveKind::Segment);
    CHECK(line.shape == glm::vec4{10.0f, 10.0f, 30.0f, 20.0f});
    CHECK(line.min == glm::vec2{9.0f, 9.0f});
    CHECK(line.max == glm::vec2{31.0f, 21.0f});

    // A circle is a rectangle rounded all the way.
    const core::Primitive& disc = data.primitives[1];
    CHECK(disc.kind == core::PrimitiveKind::Rect);
    CHECK(disc.min == glm::vec2{90.0f, 40.0f});
    CHECK(disc.shape == glm::vec4{10.0f});
    // Its outline straddles the radius.
    const core::Primitive& ring = data.primitives[2];
    CHECK(ring.min == glm::vec2{89.0f, 39.0f});
    CHECK(ring.shape == glm::vec4{11.0f});
    CHECK(ring.thickness == 2.0f);

    const core::Primitive& arc = data.primitives[3];
    CHECK(arc.kind == core::PrimitiveKind::Arc);
    CHECK(arc.shape == glm::vec4{100.0f, 50.0f, 20.0f, 0.0f});
    CHECK(arc.extra.x == 0.0f);
    CHECK(arc.extra.y == 1.5f);

    const core::Primitive& triangle = data.primitives[4];
    CHECK(triangle.kind == core::PrimitiveKind::Triangle);
    CHECK(triangle.max == glm::vec2{10.0f, 8.0f});
    CHECK(glm::vec2{triangle.extra} == glm::vec2{5.0f, 8.0f});
    CHECK(data.primitives[5].kind == core::PrimitiveKind::Triangle);
    CHECK(data.primitives[6].kind == core::PrimitiveKind::Triangle);
    // An outlined quad is its four sides, a polyline its segments.
    for (std::size_t i = 7; i < data.primitives.size(); ++i) CHECK(data.primitives[i].kind == core::PrimitiveKind::Segment);
}

TEST_CASE("Clips nest, and what falls outside one is dropped", "[ui-core]") {
    core::Context ui;
    glm::vec2 inner_min{};
    glm::vec2 inner_max{};
    const core::DrawData& data = drawn(ui, [&](core::DrawList& dl) {
        dl.push_clip({20.0f, 20.0f}, {120.0f, 80.0f});
        dl.push_clip({100.0f, 0.0f}, {300.0f, 50.0f});
        inner_min = dl.clip_min();
        inner_max = dl.clip_max();
        dl.rect_filled({110.0f, 30.0f}, {115.0f, 35.0f}, RED);  // inside both
        dl.rect_filled({10.0f, 30.0f}, {15.0f, 35.0f}, RED);    // left of the inner clip
        dl.pop_clip();
        dl.rect_filled({30.0f, 60.0f}, {40.0f, 70.0f}, RED);    // inside the outer
        dl.pop_clip();
        dl.rect_filled({150.0f, 90.0f}, {160.0f, 95.0f}, RED);  // the root's
        dl.rect_filled({300.0f, 10.0f}, {310.0f, 20.0f}, RED);  // off the root
        dl.rect_filled({0.0f, 0.0f}, {10.0f, 10.0f}, Color{1.0f, 0.0f, 0.0f, 0.0f}); // clear
    });
    CHECK(inner_min == glm::vec2{100.0f, 20.0f});
    CHECK(inner_max == glm::vec2{120.0f, 50.0f});
    REQUIRE(data.clips.size() == 3);
    CHECK(data.clips[0].max == glm::vec2{200.0f, 100.0f});
    REQUIRE(data.primitives.size() == 3);
    CHECK(data.primitives[0].clip == 2);
    CHECK(data.primitives[1].clip == 1);
    CHECK(data.primitives[2].clip == 0);
}

TEST_CASE("Pictures are listed once a frame and drawn through their index", "[ui-core]") {
    core::Context ui;
    const ftk::ui::Texture thumbnail{42};
    const ftk::ui::Texture icon{7};
    const core::DrawData& data = drawn(ui, [&](core::DrawList& dl) {
        dl.image(thumbnail, {0.0f, 0.0f}, {32.0f, 32.0f}, 4.0f);
        dl.image(icon, {40.0f, 0.0f}, {56.0f, 16.0f});
        dl.image(thumbnail, {60.0f, 0.0f}, {92.0f, 32.0f});
        dl.image({}, {0.0f, 0.0f}, {8.0f, 8.0f});  // none: nothing drawn
    });
    REQUIRE(data.textures.size() == 3);
    CHECK(data.textures[1] == thumbnail);
    CHECK(data.textures[2] == icon);
    REQUIRE(data.primitives.size() == 3);
    CHECK(data.primitives[0].texture == 1);
    CHECK(data.primitives[1].texture == 2);
    CHECK(data.primitives[2].texture == 1);
    CHECK(data.primitives[0].shape == glm::vec4{4.0f});
    CHECK(data.primitives[0].extra == glm::vec4{0.0f, 0.0f, 1.0f, 1.0f});
}

TEST_CASE("Each frame starts each window's drawing afresh", "[ui-core]") {
    core::Context ui;
    (void)drawn(ui, [](core::DrawList& dl) {
        dl.push_clip({0.0f, 0.0f}, {10.0f, 10.0f});
        dl.rect_filled({0.0f, 0.0f}, {5.0f, 5.0f}, RED);
        dl.image(ftk::ui::Texture{3}, {0.0f, 0.0f}, {5.0f, 5.0f});
        dl.pop_clip();
    });
    const core::DrawData& data = drawn(ui, [](core::DrawList& dl) { dl.rect_filled({0.0f, 0.0f}, {5.0f, 5.0f}, BLUE); });
    REQUIRE(data.primitives.size() == 1);
    CHECK(data.primitives[0].colour == core::pack_colour(BLUE));
    CHECK(data.clips.size() == 1);
    CHECK(data.textures.size() == 1);
}
