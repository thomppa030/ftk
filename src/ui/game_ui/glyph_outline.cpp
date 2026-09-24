#include "ui/game_ui/glyph_outline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

namespace fjell {

namespace {

using msdfgen::Point2;
using msdfgen::Vector2;

// Lengths are in em. A glyph spans about one and a font unit is a thousandth
// or two of that, so these sit far below anything a font draws. Edges meet
// only where they truly touch; but where two curves touch rather than cross,
// the touch can only be placed to within a few millionths along them, so the
// points found there are one point at a coarser distance. The fill is read
// further out than both.
constexpr double MEET = 1e-9;         // edges closer than this meet
constexpr double SAME_POINT = 1e-6;   // points closer than this are one vertex
constexpr double FLAT = 1e-10;        // a curve this close to its chord is cut as a line
constexpr double SIDE = 1e-5;         // how far either side of a piece its fill is read at most
constexpr int MAX_DEPTH = 64;
// Two edges that meet this often run together rather than cross.
constexpr size_t MAX_HITS = 64;

// A Bezier edge by its control points, of degree 1 to 3.
struct Curve {
    int degree{1};
    std::array<Point2, 4> p{};

    [[nodiscard]] Point2 start() const { return p[0]; }
    [[nodiscard]] Point2 end() const { return p[degree]; }

    [[nodiscard]] Point2 at(double t) const {
        std::array<Point2, 4> q = p;
        for (int level = degree; level > 0; --level) {
            for (int i = 0; i < level; ++i) q[i] = msdfgen::mix(q[i], q[i + 1], t);
        }
        return q[0];
    }

    // The way the curve heads at t; its chord where the derivative vanishes.
    [[nodiscard]] Vector2 direction(double t) const {
        std::array<Vector2, 3> q{};
        for (int i = 0; i < degree; ++i) q[i] = p[i + 1] - p[i];
        for (int level = degree - 1; level > 0; --level) {
            for (int i = 0; i < level; ++i) q[i] = msdfgen::mix(q[i], q[i + 1], t);
        }
        return q[0].length() > MEET ? q[0] : end() - start();
    }

    // The way the curve leaves its start and arrives at its end, read from
    // the nearest control point that is not on top of the end point.
    [[nodiscard]] Vector2 start_direction() const {
        for (int i = 1; i <= degree; ++i) {
            if ((p[i] - p[0]).length() > MEET) return p[i] - p[0];
        }
        return Vector2();
    }
    [[nodiscard]] Vector2 end_direction() const {
        for (int i = degree - 1; i >= 0; --i) {
            if ((p[degree] - p[i]).length() > MEET) return p[degree] - p[i];
        }
        return Vector2();
    }

    // de Casteljau: the curve before and after t.
    void split(double t, Curve& head, Curve& tail) const {
        std::array<Point2, 4> q = p;
        head.degree = degree;
        tail.degree = degree;
        head.p[0] = q[0];
        tail.p[degree] = q[degree];
        for (int level = 1; level <= degree; ++level) {
            for (int i = 0; i <= degree - level; ++i) q[i] = msdfgen::mix(q[i], q[i + 1], t);
            head.p[level] = q[0];
            tail.p[degree - level] = q[degree - level];
        }
    }

    // The curve between t0 and t1, t0 < t1.
    [[nodiscard]] Curve between(double t0, double t1) const {
        Curve head, rest, part;
        split(t1, head, rest);
        head.split(t0 / t1, rest, part);
        return part;
    }

    void reverse() { std::reverse(p.begin(), p.begin() + degree + 1); }

    // The box around the control points, which holds the whole curve.
    void box(double& l, double& b, double& r, double& t) const {
        l = r = p[0].x;
        b = t = p[0].y;
        for (int i = 1; i <= degree; ++i) {
            l = std::min(l, p[i].x);
            r = std::max(r, p[i].x);
            b = std::min(b, p[i].y);
            t = std::max(t, p[i].y);
        }
    }

    [[nodiscard]] double size() const {
        double l, b, r, t;
        box(l, b, r, t);
        return std::max(r - l, t - b);
    }

    [[nodiscard]] bool flat() const {
        if (degree == 1) return true;
        const Vector2 chord = end() - start();
        const double length = chord.length();
        for (int i = 1; i < degree; ++i) {
            const double off = length > MEET
                ? std::fabs(msdfgen::crossProduct(chord, p[i] - start())) / length
                : (p[i] - start()).length();
            if (off > FLAT) return false;
        }
        return true;
    }
};

Curve to_curve(const msdfgen::EdgeSegment& edge) {
    Curve curve;
    curve.degree = edge.type();
    const Point2* points = edge.controlPoints();
    std::copy(points, points + curve.degree + 1, curve.p.begin());
    return curve;
}

msdfgen::EdgeHolder to_edge(const Curve& curve) {
    switch (curve.degree) {
        case 2: return msdfgen::EdgeHolder(curve.p[0], curve.p[1], curve.p[2]);
        case 3: return msdfgen::EdgeHolder(curve.p[0], curve.p[1], curve.p[2], curve.p[3]);
        default: return msdfgen::EdgeHolder(curve.p[0], curve.p[1]);
    }
}

// Where two edges meet: a parameter on each and the point.
struct Hit {
    double ta{0.0};
    double tb{0.0};
    Point2 point;
};

bool boxes_meet(const Curve& a, const Curve& b) {
    double al, ab, ar, at, bl, bb, br, bt;
    a.box(al, ab, ar, at);
    b.box(bl, bb, br, bt);
    return al <= br + MEET && bl <= ar + MEET &&
           ab <= bt + MEET && bb <= at + MEET;
}

// True when `b` lies wholly to one side of the band `a` sweeps about its
// chord, so the two cannot meet. Prunes far better than boxes do along a
// slanted edge.
bool beside(const Curve& a, const Curve& b) {
    const Vector2 chord = a.end() - a.start();
    const double length = chord.length();
    if (length <= MEET) return false;
    auto offset = [&](Point2 q) { return msdfgen::crossProduct(chord, q - a.start()) / length; };
    double a_lo = 0.0, a_hi = 0.0;
    for (int i = 1; i < a.degree; ++i) {
        a_lo = std::min(a_lo, offset(a.p[i]));
        a_hi = std::max(a_hi, offset(a.p[i]));
    }
    double b_lo = std::numeric_limits<double>::max();
    double b_hi = std::numeric_limits<double>::lowest();
    for (int i = 0; i <= b.degree; ++i) {
        b_lo = std::min(b_lo, offset(b.p[i]));
        b_hi = std::max(b_hi, offset(b.p[i]));
    }
    return b_hi < a_lo - MEET || b_lo > a_hi + MEET;
}

// Where two straight pieces meet, spanning [a0, a1] and [b0, b1] of their
// edges. Collinear pieces that overlap meet at both ends of the overlap.
void intersect_chords(const Curve& a, double a0, double a1,
                      const Curve& b, double b0, double b1, std::vector<Hit>& hits) {
    const Point2 p0 = a.start();
    const Point2 q0 = b.start();
    const Vector2 da = a.end() - p0;
    const Vector2 db = b.end() - q0;
    const double la = da.length();
    const double lb = db.length();
    auto record = [&](double sa, double sb) {
        sa = std::clamp(sa, 0.0, 1.0);
        sb = std::clamp(sb, 0.0, 1.0);
        hits.push_back({a0 + sa * (a1 - a0), b0 + sb * (b1 - b0),
                        0.5 * ((p0 + sa * da) + (q0 + sb * db))});
    };
    // Where a point falls along a piece, and whether it lies on it.
    auto along = [](Point2 q, Point2 from, Vector2 d, double length, double& s) {
        s = length > MEET ? msdfgen::dotProduct(q - from, d) / (length * length) : 0.0;
        return (from + std::clamp(s, 0.0, 1.0) * d - q).length() <= MEET;
    };

    double s = 0.0;
    if (la <= MEET) {
        if (along(p0, q0, db, lb, s)) record(0.5, s);
        return;
    }
    if (lb <= MEET) {
        if (along(q0, p0, da, la, s)) record(s, 0.5);
        return;
    }

    const Vector2 w = q0 - p0;
    const double denom = msdfgen::crossProduct(da, db);
    if (std::fabs(denom) > 1e-12 * la * lb) {
        const double sa = msdfgen::crossProduct(w, db) / denom;
        const double sb = msdfgen::crossProduct(w, da) / denom;
        const double ea = MEET / la;
        const double eb = MEET / lb;
        if (sa >= -ea && sa <= 1.0 + ea && sb >= -eb && sb <= 1.0 + eb) record(sa, sb);
        return;
    }
    // Parallel: they meet only on one line, where b's ends fall along a.
    if (std::fabs(msdfgen::crossProduct(w, da)) / la > MEET) return;
    double s0 = 0.0, s1 = 0.0;
    along(q0, p0, da, la, s0);
    along(b.end(), p0, da, la, s1);
    const double lo = std::max(0.0, std::min(s0, s1));
    const double hi = std::min(1.0, std::max(s0, s1));
    if ((hi - lo) * la < -MEET) return;
    along(p0 + lo * da, q0, db, lb, s);
    record(lo, s);
    if ((hi - lo) * la > MEET) {
        along(p0 + hi * da, q0, db, lb, s);
        record(hi, s);
    }
}

// Every point where the parts [a0, a1] of one edge and [b0, b1] of another
// meet, found by halving each until it is straight. False when they meet
// too often to be crossing: they run together.
bool intersect(const Curve& a, double a0, double a1, const Curve& b, double b0, double b1,
               int depth, std::vector<Hit>& hits) {
    if (hits.size() > MAX_HITS) return false;
    if (!boxes_meet(a, b) || beside(a, b) || beside(b, a)) return true;
    const bool a_flat = a.flat();
    const bool b_flat = b.flat();
    if ((a_flat && b_flat) || depth >= MAX_DEPTH) {
        intersect_chords(a, a0, a1, b, b0, b1, hits);
        return hits.size() <= MAX_HITS;
    }
    const double am = 0.5 * (a0 + a1);
    const double bm = 0.5 * (b0 + b1);
    Curve a_head, a_tail, b_head, b_tail;
    if (a_flat) {
        b.split(0.5, b_head, b_tail);
        return intersect(a, a0, a1, b_head, b0, bm, depth + 1, hits) &&
               intersect(a, a0, a1, b_tail, bm, b1, depth + 1, hits);
    }
    if (b_flat) {
        a.split(0.5, a_head, a_tail);
        return intersect(a_head, a0, am, b, b0, b1, depth + 1, hits) &&
               intersect(a_tail, am, a1, b, b0, b1, depth + 1, hits);
    }
    a.split(0.5, a_head, a_tail);
    b.split(0.5, b_head, b_tail);
    return intersect(a_head, a0, am, b_head, b0, bm, depth + 1, hits) &&
           intersect(a_head, a0, am, b_tail, bm, b1, depth + 1, hits) &&
           intersect(a_tail, am, a1, b_head, b0, bm, depth + 1, hits) &&
           intersect(a_tail, am, a1, b_tail, bm, b1, depth + 1, hits);
}

// Halving reports one crossing from each pair of neighbouring halves that
// holds it; keeps one hit per crossing.
std::vector<Hit> distinct_crossings(std::vector<Hit> hits) {
    std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return x.ta < y.ta; });
    std::vector<Hit> out;
    size_t first = 0;
    for (size_t i = 1; i <= hits.size(); ++i) {
        if (i < hits.size() && (hits[i].point - hits[i - 1].point).length() <= SAME_POINT) continue;
        out.push_back(hits[first + (i - first) / 2]);
        first = i;
    }
    return out;
}

int winding(const msdfgen::Shape& shape, Point2 q) {
    msdfgen::Scanline line;
    shape.scanline(line, q.y);
    return line.sumIntersections(q.x);
}

// The corners of every contour and the points where edges cross, merged
// where they coincide. A joint of the outline keeps its exact position.
class Vertices {
public:
    int add(Point2 point, bool joint) {
        points_.push_back(point);
        joints_.push_back(joint);
        parents_.push_back(static_cast<int>(parents_.size()));
        return parents_.back();
    }

    int find(int v) {
        while (parents_[v] != v) {
            parents_[v] = parents_[parents_[v]];
            v = parents_[v];
        }
        return v;
    }

    void unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (joints_[b] && !joints_[a]) std::swap(a, b);
        parents_[b] = a;
    }

    [[nodiscard]] Point2 point(int v) const { return points_[v]; }
    [[nodiscard]] size_t size() const { return parents_.size(); }

private:
    std::vector<Point2> points_;
    std::vector<bool> joints_;
    std::vector<int> parents_;
};

struct Edge {
    Curve curve;
    int from{0};
    int to{0};
    std::vector<std::pair<double, int>> cuts; // where it is crossed, by parameter
};

struct Piece {
    Curve curve;
    int from{0};
    int to{0};
};

// Records that `v` lies on `edge` at `t`: its start or end, or a cut.
void attach(Edge& edge, double t, int v, Vertices& vertices) {
    const Point2 at = edge.curve.at(t);
    if ((at - edge.curve.start()).length() <= SAME_POINT) {
        vertices.unite(v, edge.from);
    } else if ((at - edge.curve.end()).length() <= SAME_POINT) {
        vertices.unite(v, edge.to);
    } else {
        edge.cuts.emplace_back(t, v);
    }
}

// The outline chained from pieces that each have the fill on their right.
// Where several pieces leave a vertex the chain takes the sharpest right
// turn, which keeps two shapes that touch at a point apart.
bool chain(const std::vector<Piece>& pieces, size_t vertex_count, msdfgen::Shape& out) {
    std::vector<std::vector<size_t>> leaving(vertex_count);
    for (size_t i = 0; i < pieces.size(); ++i) leaving[pieces[i].from].push_back(i);
    std::vector<bool> used(pieces.size(), false);
    for (size_t first = 0; first < pieces.size(); ++first) {
        if (used[first]) continue;
        auto& contour = out.addContour();
        size_t current = first;
        while (true) {
            used[current] = true;
            contour.addEdge(to_edge(pieces[current].curve));
            const int at = pieces[current].to;
            if (at == pieces[first].from) break;
            const Vector2 in = pieces[current].curve.end_direction().normalize(true);
            size_t next = pieces.size();
            double sharpest = std::numeric_limits<double>::max();
            for (size_t candidate : leaving[at]) {
                if (used[candidate]) continue;
                const Vector2 out_dir = pieces[candidate].curve.start_direction().normalize(true);
                const double turn = std::atan2(msdfgen::crossProduct(in, out_dir),
                                               msdfgen::dotProduct(in, out_dir));
                if (turn < sharpest) {
                    sharpest = turn;
                    next = candidate;
                }
            }
            if (next == pieces.size()) return false;
            current = next;
        }
    }
    return true;
}

double distance_to_outline(const msdfgen::Shape& shape, Point2 q) {
    double nearest = std::numeric_limits<double>::max();
    for (const auto& contour : shape.contours) {
        for (const auto& edge : contour.edges) {
            double param = 0.0;
            nearest = std::min(nearest, std::fabs(edge->signedDistance(q, param).distance));
        }
    }
    return nearest;
}

// Whether `merged` fills what `original` fills, with no point covered twice.
// Read over a grid across the glyph and just either side of every piece,
// where a wrong chain would show first. A point on the outline is on
// neither side, and font coordinates are regular enough that a grid lands
// on one, so those are skipped.
bool fills_the_same(const msdfgen::Shape& original, const msdfgen::Shape& merged,
                    const std::vector<Piece>& pieces) {
    auto agrees = [&](Point2 q, int original_winding, int merged_winding) {
        if ((original_winding != 0) == (merged_winding != 0) && std::abs(merged_winding) <= 1) return true;
        return distance_to_outline(original, q) < SIDE;
    };
    // One scanline per row serves every point along it.
    const auto bounds = original.getBounds();
    constexpr int COLUMNS = 41;
    constexpr int ROWS = 43;
    msdfgen::Scanline original_row;
    msdfgen::Scanline merged_row;
    for (int y = 0; y < ROWS; ++y) {
        const double row_y = bounds.b + (y + 0.5) / ROWS * (bounds.t - bounds.b);
        original.scanline(original_row, row_y);
        merged.scanline(merged_row, row_y);
        for (int x = 0; x < COLUMNS; ++x) {
            const Point2 q(bounds.l + (x + 0.5) / COLUMNS * (bounds.r - bounds.l), row_y);
            if (!agrees(q, original_row.sumIntersections(q.x), merged_row.sumIntersections(q.x))) return false;
        }
    }
    for (const auto& piece : pieces) {
        const Vector2 normal = piece.curve.direction(0.5).getOrthonormal(true, true);
        const Point2 mid = piece.curve.at(0.5);
        for (const Point2 q : {mid + 8.0 * SIDE * normal, mid - 8.0 * SIDE * normal}) {
            if (!agrees(q, winding(original, q), winding(merged, q))) return false;
        }
    }
    return true;
}

} // namespace

bool merge_overlapping_contours(msdfgen::Shape& shape) {
    Vertices vertices;
    std::vector<Edge> edges;
    for (const auto& contour : shape.contours) {
        const size_t count = contour.edges.size();
        if (count == 0) continue;
        std::vector<int> joints(count);
        for (size_t k = 0; k < count; ++k) joints[k] = vertices.add(contour.edges[k]->point(0), true);
        for (size_t k = 0; k < count; ++k) {
            Edge edge;
            edge.curve = to_curve(*contour.edges[k]);
            edge.from = joints[k];
            edge.to = joints[(k + 1) % count];
            // An edge of no length only joins its two ends.
            if (edge.curve.size() <= SAME_POINT) {
                vertices.unite(edge.from, edge.to);
                continue;
            }
            edges.push_back(std::move(edge));
        }
    }

    for (size_t i = 0; i < edges.size(); ++i) {
        for (size_t j = i + 1; j < edges.size(); ++j) {
            std::vector<Hit> hits;
            if (!intersect(edges[i].curve, 0.0, 1.0, edges[j].curve, 0.0, 1.0, 0, hits)) return false;
            for (const auto& hit : distinct_crossings(std::move(hits))) {
                const int v = vertices.add(hit.point, false);
                attach(edges[i], hit.ta, v, vertices);
                attach(edges[j], hit.tb, v, vertices);
            }
        }
    }

    // One cut per crossing along each edge, however many edges crossed there.
    bool changed = false;
    for (auto& edge : edges) {
        std::sort(edge.cuts.begin(), edge.cuts.end());
        std::vector<std::pair<double, int>> cuts;
        for (const auto& cut : edge.cuts) {
            if (!cuts.empty() &&
                (edge.curve.at(cut.first) - edge.curve.at(cuts.back().first)).length() <= SAME_POINT) {
                vertices.unite(cuts.back().second, cut.second);
                continue;
            }
            cuts.push_back(cut);
        }
        edge.cuts = std::move(cuts);
        changed = changed || !edge.cuts.empty();
    }

    // Cut every edge at its crossings.
    std::vector<Piece> cut;
    for (const auto& edge : edges) {
        std::vector<std::pair<double, int>> stops;
        stops.emplace_back(0.0, edge.from);
        stops.insert(stops.end(), edge.cuts.begin(), edge.cuts.end());
        stops.emplace_back(1.0, edge.to);
        for (size_t k = 0; k + 1 < stops.size(); ++k) {
            Piece piece;
            piece.curve = stops.size() == 2 ? edge.curve
                                            : edge.curve.between(stops[k].first, stops[k + 1].first);
            piece.from = vertices.find(stops[k].second);
            piece.to = vertices.find(stops[k + 1].second);
            piece.curve.p[0] = vertices.point(piece.from);
            piece.curve.p[piece.curve.degree] = vertices.point(piece.to);
            if (piece.curve.size() <= SAME_POINT) {
                changed = true;
                continue;
            }
            cut.push_back(piece);
        }
    }

    // Keep the pieces that part fill from empty space, each turned to have
    // the fill on its right. The fill is read either side of a piece's
    // middle, nearer than any other piece passes, or a short piece where two
    // edges cross at a shallow angle would be read beyond the other edge.
    std::vector<Piece> pieces;
    for (size_t index = 0; index < cut.size(); ++index) {
        Piece piece = cut[index];
        const Point2 mid = piece.curve.at(0.5);
        double clear = 2.0 * SIDE;
        for (size_t other = 0; other < cut.size(); ++other) {
            if (other == index) continue;
            const Piece& near = cut[other];
            // A twin, the same stretch of outline from another contour, lies
            // along the piece; the fill is read past it.
            const bool same_ends = (near.from == piece.from && near.to == piece.to) ||
                                   (near.from == piece.to && near.to == piece.from);
            if (same_ends && (near.curve.at(0.5) - mid).length() <= SAME_POINT) continue;
            double l, b, r, t;
            near.curve.box(l, b, r, t);
            if (mid.x < l - clear || mid.x > r + clear || mid.y < b - clear || mid.y > t + clear) continue;
            double param = 0.0;
            clear = std::min(clear, std::fabs(to_edge(near.curve)->signedDistance(mid, param).distance));
        }
        const double reach = std::max(0.5 * clear, MEET);
        const Vector2 left = piece.curve.direction(0.5).getOrthonormal(true, true);
        const bool fill_left = winding(shape, mid + reach * left) != 0;
        const bool fill_right = winding(shape, mid - reach * left) != 0;
        if (fill_left == fill_right) {
            changed = true;
            continue;
        }
        if (fill_left) {
            piece.curve.reverse();
            std::swap(piece.from, piece.to);
            changed = true;
        }
        pieces.push_back(piece);
    }

    // Two contours that share a stretch of outline both keep their copy of
    // it; one is enough.
    std::vector<Piece> kept;
    for (const auto& piece : pieces) {
        const bool twin = std::any_of(kept.begin(), kept.end(), [&](const Piece& other) {
            if (other.from != piece.from || other.to != piece.to || other.curve.degree != piece.curve.degree) {
                return false;
            }
            for (int i = 0; i <= piece.curve.degree; ++i) {
                if ((other.curve.p[i] - piece.curve.p[i]).length() > SAME_POINT) return false;
            }
            return true;
        });
        if (twin) {
            changed = true;
            continue;
        }
        kept.push_back(piece);
    }

    if (!changed) return true;

    msdfgen::Shape merged;
    merged.inverseYAxis = shape.inverseYAxis;
    if (!chain(kept, vertices.size(), merged) || !merged.validate()) return false;
    if (!fills_the_same(shape, merged, kept)) return false;
    shape = std::move(merged);
    return true;
}

} // namespace fjell
