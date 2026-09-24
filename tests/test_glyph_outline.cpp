#include "ui/game_ui/glyph_outline.hpp"

#include "outline_reference.hpp"

#include <catch2/catch_test_macros.hpp>

#include <msdfgen.h>

#include <cstddef>
#include <vector>

using namespace fjell;
using namespace fjell::test;
using msdfgen::Point2;

namespace {

// A rectangle, wound clockwise (fill on the right, as TrueType draws an
// outline) or counter-clockwise (as a hole in one, or as CFF fonts draw).
void add_rect(msdfgen::Shape& shape, double l, double b, double r, double t, bool clockwise = true) {
    auto& contour = shape.addContour();
    const Point2 corners[4] = {{l, b}, {l, t}, {r, t}, {r, b}};
    for (int i = 0; i < 4; ++i) {
        const int from = clockwise ? i : 3 - i;
        const int to = clockwise ? (i + 1) % 4 : (6 - i) % 4;
        contour.addEdge(msdfgen::EdgeHolder(corners[from], corners[to]));
    }
}

// A circle of four cubic arcs, wound clockwise unless told otherwise.
void add_circle(msdfgen::Shape& shape, Point2 centre, double radius, bool clockwise = true) {
    constexpr double KAPPA = 0.5522847498;
    const double k = KAPPA * radius;
    const Point2 e(centre.x + radius, centre.y);
    const Point2 n(centre.x, centre.y + radius);
    const Point2 w(centre.x - radius, centre.y);
    const Point2 s(centre.x, centre.y - radius);
    auto& contour = shape.addContour();
    if (clockwise) {
        contour.addEdge(msdfgen::EdgeHolder(e, Point2(e.x, e.y - k), Point2(s.x + k, s.y), s));
        contour.addEdge(msdfgen::EdgeHolder(s, Point2(s.x - k, s.y), Point2(w.x, w.y - k), w));
        contour.addEdge(msdfgen::EdgeHolder(w, Point2(w.x, w.y + k), Point2(n.x - k, n.y), n));
        contour.addEdge(msdfgen::EdgeHolder(n, Point2(n.x + k, n.y), Point2(e.x, e.y + k), e));
    } else {
        contour.addEdge(msdfgen::EdgeHolder(e, Point2(e.x, e.y + k), Point2(n.x + k, n.y), n));
        contour.addEdge(msdfgen::EdgeHolder(n, Point2(n.x - k, n.y), Point2(w.x, w.y + k), w));
        contour.addEdge(msdfgen::EdgeHolder(w, Point2(w.x, w.y - k), Point2(s.x - k, s.y), s));
        contour.addEdge(msdfgen::EdgeHolder(s, Point2(s.x + k, s.y), Point2(e.x, e.y - k), e));
    }
}

size_t edge_count(const msdfgen::Shape& shape) {
    size_t count = 0;
    for (const auto& contour : shape.contours) count += contour.edges.size();
    return count;
}

// What a merged outline must be: it fills what the original filled, and
// every stretch of it has the fill on its right and nothing on its left.
void require_merged_outline_of(const msdfgen::Shape& merged, const msdfgen::Shape& original) {
    REQUIRE(merged.validate());
    const auto before = flatten(original);
    const auto after = flatten(merged);
    const auto bounds = original.getBounds();
    int different = 0;
    for (int y = 0; y < 37; ++y) {
        for (int x = 0; x < 31; ++x) {
            const Point2 p(bounds.l - 0.05 + (x + 0.5) / 31 * (bounds.r - bounds.l + 0.1),
                           bounds.b - 0.05 + (y + 0.5) / 37 * (bounds.t - bounds.b + 0.1));
            if (distance_to(before, p) < 1e-4) continue;
            if (filled(before, p) != filled(after, p)) ++different;
        }
    }
    CHECK(different == 0);
    int wrong_side = 0;
    for (const auto& segment : after) {
        const Sides sides = fill_beside(after, segment);
        if (sides.left || !sides.right) ++wrong_side;
    }
    CHECK(wrong_side == 0);
}

} // namespace

TEST_CASE("Overlapping contours merge into one outline", "[fjfont]") {
    msdfgen::Shape shape;
    add_rect(shape, 0.0, 0.0, 0.5, 0.5);
    add_rect(shape, 0.3, 0.3, 0.8, 0.8);
    const msdfgen::Shape original = shape;

    REQUIRE(merge_overlapping_contours(shape));
    CHECK(shape.contours.size() == 1);
    // Each square loses the two sides that ran inside the other, and the
    // two that cross are cut where they meet.
    CHECK(edge_count(shape) == 8);
    require_merged_outline_of(shape, original);
}

TEST_CASE("A contour inside another that fills the same way is dropped", "[fjfont]") {
    msdfgen::Shape shape;
    add_rect(shape, 0.0, 0.0, 1.0, 1.0);
    add_rect(shape, 0.25, 0.25, 0.75, 0.75);
    const msdfgen::Shape original = shape;

    REQUIRE(merge_overlapping_contours(shape));
    CHECK(shape.contours.size() == 1);
    CHECK(edge_count(shape) == 4);
    require_merged_outline_of(shape, original);
}

TEST_CASE("An outline with nothing to merge is left exactly as it was", "[fjfont]") {
    // A ring: the hole is wound the other way and cuts the fill, it does
    // not overlap it.
    msdfgen::Shape shape;
    add_circle(shape, Point2(0.5, 0.5), 0.5);
    add_circle(shape, Point2(0.5, 0.5), 0.3, false);
    const msdfgen::Shape original = shape;

    REQUIRE(merge_overlapping_contours(shape));
    REQUIRE(shape.contours.size() == original.contours.size());
    for (size_t c = 0; c < shape.contours.size(); ++c) {
        REQUIRE(shape.contours[c].edges.size() == original.contours[c].edges.size());
        for (size_t e = 0; e < shape.contours[c].edges.size(); ++e) {
            const auto& edge = *shape.contours[c].edges[e];
            const auto& was = *original.contours[c].edges[e];
            REQUIRE(edge.type() == was.type());
            for (int i = 0; i <= edge.type(); ++i) {
                CHECK(edge.controlPoints()[i] == was.controlPoints()[i]);
            }
        }
    }
}

TEST_CASE("An outline wound the other way turns to have its fill on the right", "[fjfont]") {
    msdfgen::Shape shape;
    add_rect(shape, 0.0, 0.0, 1.0, 0.5, false);
    const msdfgen::Shape original = shape;

    REQUIRE(merge_overlapping_contours(shape));
    CHECK(shape.contours.size() == 1);
    CHECK(edge_count(shape) == 4);
    require_merged_outline_of(shape, original);
}

TEST_CASE("Contours that share a side merge across it", "[fjfont]") {
    msdfgen::Shape shape;
    add_rect(shape, 0.0, 0.0, 0.5, 0.5);
    add_rect(shape, 0.5, 0.0, 1.0, 0.5);
    const msdfgen::Shape original = shape;

    REQUIRE(merge_overlapping_contours(shape));
    CHECK(shape.contours.size() == 1);
    // The shared side is gone; top and bottom stay in two lengths each.
    CHECK(edge_count(shape) == 6);
    require_merged_outline_of(shape, original);
}

TEST_CASE("Contours that touch at a corner stay two outlines", "[fjfont]") {
    msdfgen::Shape shape;
    add_rect(shape, 0.0, 0.0, 0.5, 0.5);
    add_rect(shape, 0.5, 0.5, 1.0, 1.0);
    add_rect(shape, 0.2, 0.2, 0.3, 0.3); // inside the first, so there is something to merge
    const msdfgen::Shape original = shape;

    REQUIRE(merge_overlapping_contours(shape));
    CHECK(shape.contours.size() == 2);
    require_merged_outline_of(shape, original);
}

TEST_CASE("A bowl laid over a stem merges with its curves kept", "[fjfont]") {
    // The way many fonts draw a b: a ring whose outer edge runs into the stem.
    msdfgen::Shape shape;
    add_rect(shape, 0.0, 0.0, 0.12, 1.0);
    add_circle(shape, Point2(0.35, 0.3), 0.3);
    add_circle(shape, Point2(0.35, 0.3), 0.2, false);
    const msdfgen::Shape original = shape;

    REQUIRE(merge_overlapping_contours(shape));
    // The stem and the bowl's outside become one outline; the counter stays
    // a hole of its own.
    CHECK(shape.contours.size() == 2);
    size_t cubics = 0;
    for (const auto& contour : shape.contours) {
        for (const auto& edge : contour.edges) cubics += edge->type() == 3 ? 1 : 0;
    }
    CHECK(cubics >= 6);
    require_merged_outline_of(shape, original);
}

TEST_CASE("Contours that cross at a shallow angle just short of a corner merge", "[fjfont]") {
    // The stroke crosses the bar's top a twentieth of a font unit before the
    // bar's corner, so between the crossing and the corner lies a sliver of
    // the bar's outline inside the stroke, a hair from the stroke's edge.
    msdfgen::Shape shape;
    add_rect(shape, 0.0, 0.0, 1.0, 0.2);
    auto& stroke = shape.addContour();
    const Point2 low_left(0.49995, 0.1738);
    const Point2 high_right(1.49995, 0.2262);
    const Point2 below(1.5, -0.5);
    stroke.addEdge(msdfgen::EdgeHolder(low_left, high_right));
    stroke.addEdge(msdfgen::EdgeHolder(high_right, below));
    stroke.addEdge(msdfgen::EdgeHolder(below, low_left));
    const msdfgen::Shape original = shape;

    REQUIRE(merge_overlapping_contours(shape));
    CHECK(shape.contours.size() == 1);
    require_merged_outline_of(shape, original);
}

TEST_CASE("A corner that sits a hair outside another contour merges", "[fjfont]") {
    // As an ogonek meets the bowl of a U: the corner lies a fraction of a
    // millionth of an em below the bowl, and the edge leaving it crosses
    // into the bowl straight away.
    msdfgen::Shape shape;
    add_circle(shape, Point2(0.5, 0.5), 0.5);
    auto& tail = shape.addContour();
    const Point2 a(0.3, -0.11);
    const Point2 corner(0.50049, 0.0);
    const Point2 b(0.7, 0.11);
    const Point2 c(0.6, -0.3);
    tail.addEdge(msdfgen::EdgeHolder(a, corner));
    tail.addEdge(msdfgen::EdgeHolder(corner, b));
    tail.addEdge(msdfgen::EdgeHolder(b, c));
    tail.addEdge(msdfgen::EdgeHolder(c, a));
    const msdfgen::Shape original = shape;

    REQUIRE(merge_overlapping_contours(shape));
    CHECK(shape.contours.size() == 1);
    require_merged_outline_of(shape, original);
}
