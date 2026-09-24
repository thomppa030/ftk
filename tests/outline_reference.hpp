#pragma once

// A glyph outline's fill and boundary, worked out independently of the font
// baker by cutting the outline into short straight segments, for tests to
// hold baked atlases and merged outlines against.

#include <msdfgen.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace fjell::test {

struct Segment {
    msdfgen::Point2 a;
    msdfgen::Point2 b;
};

// Fine enough that the error is a small fraction of an atlas texel.
inline std::vector<Segment> flatten(const msdfgen::Shape& shape) {
    constexpr int STEPS = 48;
    std::vector<Segment> segments;
    for (const auto& contour : shape.contours) {
        for (const auto& edge : contour.edges) {
            for (int i = 0; i < STEPS; ++i) {
                segments.push_back({edge->point(static_cast<double>(i) / STEPS),
                                    edge->point(static_cast<double>(i + 1) / STEPS)});
            }
        }
    }
    return segments;
}

// The outline's winding number around `p`. Nonzero is how FreeType fills a
// glyph, and the rule under which overlapping contours read as one shape.
inline int winding_number(const std::vector<Segment>& outline, msdfgen::Point2 p) {
    int winding = 0;
    for (const auto& s : outline) {
        const double side = msdfgen::crossProduct(s.b - s.a, p - s.a);
        if (s.a.y <= p.y) {
            if (s.b.y > p.y && side > 0) ++winding;
        } else if (s.b.y <= p.y && side < 0) {
            --winding;
        }
    }
    return winding;
}

inline bool filled(const std::vector<Segment>& outline, msdfgen::Point2 p) {
    return winding_number(outline, p) != 0;
}

// Which side of a segment the fill is on, read a hair to either side of its
// middle.
struct Sides {
    bool left{false};
    bool right{false};
};

inline Sides fill_beside(const std::vector<Segment>& outline, const Segment& s) {
    constexpr double HAIR = 1e-6;
    const msdfgen::Vector2 left = (s.b - s.a).getOrthonormal(true);
    const msdfgen::Point2 mid = 0.5 * (s.a + s.b);
    return {filled(outline, mid + HAIR * left), filled(outline, mid - HAIR * left)};
}

// The segments that part fill from empty space. Overlapping contours leave
// segments with fill on both sides: edges that are not part of what is drawn.
inline std::vector<Segment> boundary(const std::vector<Segment>& outline) {
    std::vector<Segment> out;
    for (const auto& s : outline) {
        const Sides sides = fill_beside(outline, s);
        if (sides.left != sides.right) out.push_back(s);
    }
    return out;
}

inline double distance_to(const std::vector<Segment>& segments, msdfgen::Point2 p) {
    double nearest = std::numeric_limits<double>::max();
    for (const auto& s : segments) {
        const msdfgen::Vector2 ab = s.b - s.a;
        const double length_sq = msdfgen::dotProduct(ab, ab);
        const double t = length_sq > 0.0
            ? std::clamp(msdfgen::dotProduct(p - s.a, ab) / length_sq, 0.0, 1.0)
            : 0.0;
        nearest = std::min(nearest, (p - (s.a + t * ab)).length());
    }
    return nearest;
}

} // namespace fjell::test
