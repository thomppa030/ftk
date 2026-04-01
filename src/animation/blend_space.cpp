#include "animation/blend_space.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace fjell {

// ── BlendSpace1D ────────────────────────────────────────────────────────

void BlendSpace1D::add(BlendSpacePoint point) {
    float pos = point.position;
    auto it = std::ranges::lower_bound(points_, pos, {}, &BlendSpacePoint::position);
    points_.insert(it, std::move(point));
}

BlendResult BlendSpace1D::evaluate(float param) const {
    if (points_.empty()) return {};

    if (points_.size() == 1) {
        return {0, 0, 1.0f, 0.0f};
    }

    if (param <= points_.front().position) {
        return {0, 0, 1.0f, 0.0f};
    }
    if (param >= points_.back().position) {
        auto last = static_cast<uint32_t>(points_.size() - 1);
        return {last, last, 1.0f, 0.0f};
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(points_.size()) - 1; ++i) {
        if (param >= points_[i].position && param <= points_[i + 1].position) {
            float range = points_[i + 1].position - points_[i].position;
            float t = (range > 0.0f) ? (param - points_[i].position) / range : 0.0f;
            return {i, i + 1, 1.0f - t, t};
        }
    }

    return {0, 0, 1.0f, 0.0f};
}

Result<BlendSpace1D> BlendSpace1D::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) return make_error("Failed to open .fjblend1D: " + path);

    auto j = nlohmann::json::parse(file);
    BlendSpace1D bs;
    bs.parameter_ = j.value("parameter", "");
    bs.skeleton_path_ = j.value("skeleton", "");
    bs.damp_speed_ = j.value("damp_speed", 10.0f);

    if (j.contains("points") && j["points"].is_array()) {
        for (const auto& jp : j["points"]) {
            BlendSpacePoint pt;
            pt.clip = jp.value("clip", "");
            pt.position = jp.value("position", 0.0f);
            bs.add(std::move(pt));
        }
    }

    FJELL_CORE_INFO("Loaded blend space 1D: {} ({} points, param='{}')",
                    path, bs.size(), bs.parameter_);
    return bs;
}

Result<> BlendSpace1D::save(const std::string& path) const {
    nlohmann::json j;
    if (!skeleton_path_.empty()) j["skeleton"] = skeleton_path_;
    j["parameter"] = parameter_;
    if (damp_speed_ != 10.0f) j["damp_speed"] = damp_speed_;
    j["points"] = nlohmann::json::array();

    for (const auto& pt : points_) {
        j["points"].push_back({{"clip", pt.clip}, {"position", pt.position}});
    }

    std::ofstream file(path);
    if (!file) return make_error("Failed to save .fjblend1D: " + path);
    file << j.dump(2);
    return {};
}

// ── BlendSpace2D ────────────────────────────────────────────────────────

void BlendSpace2D::add(BlendSpacePoint2D point) {
    points_.push_back(std::move(point));
}

// Bowyer-Watson incremental Delaunay triangulation.
// Works correctly with interior points (e.g. idle at center of a blend space).
void BlendSpace2D::triangulate() {
    triangles_.clear();
    uint32_t n = static_cast<uint32_t>(points_.size());
    if (n < 3) return;

    // Work with indexed 2D positions
    struct Tri { int a, b, c; };
    std::vector<Tri> tris;

    // Super-triangle that contains all points
    float min_x = points_[0].position.x, max_x = min_x;
    float min_y = points_[0].position.y, max_y = min_y;
    for (const auto& p : points_) {
        min_x = std::min(min_x, p.position.x);
        max_x = std::max(max_x, p.position.x);
        min_y = std::min(min_y, p.position.y);
        max_y = std::max(max_y, p.position.y);
    }
    float dx = max_x - min_x, dy = max_y - min_y;
    float dmax = std::max(dx, dy) * 2.0f;
    float cx = (min_x + max_x) * 0.5f, cy = (min_y + max_y) * 0.5f;

    // Super-triangle vertices stored at indices n, n+1, n+2
    glm::vec2 super[3] = {
        {cx - dmax, cy - dmax},
        {cx + dmax, cy - dmax},
        {cx,        cy + dmax * 2.0f},
    };
    int sn = static_cast<int>(n);
    tris.push_back({sn, sn + 1, sn + 2});

    auto get_pos = [&](int idx) -> glm::vec2 {
        if (idx >= sn) return super[idx - sn];
        return points_[idx].position;
    };

    // Circumcircle test
    auto in_circumcircle = [&](const Tri& t, glm::vec2 p) -> bool {
        glm::vec2 a = get_pos(t.a), b = get_pos(t.b), c = get_pos(t.c);
        float ax_ = a.x - p.x, ay_ = a.y - p.y;
        float bx_ = b.x - p.x, by_ = b.y - p.y;
        float cx_ = c.x - p.x, cy_ = c.y - p.y;
        float det = ax_ * (by_ * (cx_ * cx_ + cy_ * cy_) - cy_ * (bx_ * bx_ + by_ * by_))
                   - bx_ * (ay_ * (cx_ * cx_ + cy_ * cy_) - cy_ * (ax_ * ax_ + ay_ * ay_))
                   + cx_ * (ay_ * (bx_ * bx_ + by_ * by_) - by_ * (ax_ * ax_ + ay_ * ay_));
        // CCW winding → det > 0 means inside
        // Ensure CCW: check cross product
        float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        return cross > 0 ? det > 0 : det < 0;
    };

    // Insert each point
    for (int i = 0; i < sn; ++i) {
        glm::vec2 p = points_[i].position;

        // Find bad triangles (point inside circumcircle)
        std::vector<Tri> bad;
        std::vector<Tri> good;
        for (const auto& t : tris) {
            if (in_circumcircle(t, p))
                bad.push_back(t);
            else
                good.push_back(t);
        }

        // Find boundary polygon (edges not shared by two bad triangles)
        struct Edge { int a, b; };
        std::vector<Edge> boundary;
        for (const auto& t : bad) {
            Edge edges[3] = {{t.a, t.b}, {t.b, t.c}, {t.c, t.a}};
            for (const auto& e : edges) {
                bool shared = false;
                for (const auto& t2 : bad) {
                    if (&t2 == &t) continue;
                    int v[3] = {t2.a, t2.b, t2.c};
                    for (int j = 0; j < 3; ++j) {
                        if (v[j] == e.b && v[(j + 1) % 3] == e.a) { shared = true; break; }
                    }
                    if (shared) break;
                }
                if (!shared) boundary.push_back(e);
            }
        }

        // Re-triangulate with new point
        tris = std::move(good);
        for (const auto& e : boundary) {
            tris.push_back({e.a, e.b, i});
        }
    }

    // Remove triangles that reference super-triangle vertices
    for (const auto& t : tris) {
        if (t.a >= sn || t.b >= sn || t.c >= sn) continue;
        triangles_.push_back({static_cast<uint32_t>(t.a),
                              static_cast<uint32_t>(t.b),
                              static_cast<uint32_t>(t.c)});
    }
}

BlendResult2D BlendSpace2D::evaluate(glm::vec2 param) const {
    if (points_.empty()) return {};

    if (points_.size() == 1) {
        return {{0, 0, 0}, {1.0f, 0.0f, 0.0f}, 1};
    }

    if (points_.size() == 2) {
        // Lerp between the two points based on projection
        glm::vec2 d = points_[1].position - points_[0].position;
        float len2 = glm::dot(d, d);
        float t = (len2 > 0.0f) ? glm::clamp(glm::dot(param - points_[0].position, d) / len2, 0.0f, 1.0f) : 0.0f;
        return {{0, 1, 0}, {1.0f - t, t, 0.0f}, 2};
    }

    // Find containing triangle via barycentric coordinates
    for (const auto& tri : triangles_) {
        const glm::vec2& a = points_[tri.a].position;
        const glm::vec2& b = points_[tri.b].position;
        const glm::vec2& c = points_[tri.c].position;

        glm::vec2 v0 = b - a, v1 = c - a, v2 = param - a;
        float d00 = glm::dot(v0, v0);
        float d01 = glm::dot(v0, v1);
        float d11 = glm::dot(v1, v1);
        float d20 = glm::dot(v2, v0);
        float d21 = glm::dot(v2, v1);
        float denom = d00 * d11 - d01 * d01;
        if (std::abs(denom) < 1e-8f) continue;

        float v = (d11 * d20 - d01 * d21) / denom;
        float w = (d00 * d21 - d01 * d20) / denom;
        float u = 1.0f - v - w;

        if (u >= -0.01f && v >= -0.01f && w >= -0.01f) {
            // Clamp and renormalize
            u = std::max(0.0f, u);
            v = std::max(0.0f, v);
            w = std::max(0.0f, w);
            float sum = u + v + w;
            return {{tri.a, tri.b, tri.c}, {u / sum, v / sum, w / sum}, 3};
        }
    }

    // Outside all triangles — project onto the nearest triangle edge to get a
    // smooth blend instead of snapping to the nearest vertex.
    // For each triangle edge, find the closest point on the segment and track
    // the best (closest) one.
    float best_dist2 = std::numeric_limits<float>::max();
    BlendResult2D best_result = {{0, 0, 0}, {1.0f, 0.0f, 0.0f}, 1};

    for (const auto& tri : triangles_) {
        uint32_t edge_indices[3][2] = {{tri.a, tri.b}, {tri.b, tri.c}, {tri.c, tri.a}};
        // Third vertex (opposite the edge) for barycentric weight assignment
        uint32_t opposite[3] = {tri.c, tri.a, tri.b};

        for (int e = 0; e < 3; ++e) {
            uint32_t i0 = edge_indices[e][0];
            uint32_t i1 = edge_indices[e][1];
            const glm::vec2& p0 = points_[i0].position;
            const glm::vec2& p1 = points_[i1].position;

            glm::vec2 edge = p1 - p0;
            float len2 = glm::dot(edge, edge);
            float t = (len2 > 0.0f) ? glm::clamp(glm::dot(param - p0, edge) / len2, 0.0f, 1.0f) : 0.0f;

            glm::vec2 proj = p0 + t * edge;
            float dist2 = glm::dot(param - proj, param - proj);

            if (dist2 < best_dist2) {
                best_dist2 = dist2;
                // On this edge, weight splits between i0 and i1; opposite vertex gets 0
                float w0 = 1.0f - t;
                float w1 = t;
                best_result = {{i0, i1, opposite[e]}, {w0, w1, 0.0f}, (w0 > 0.0f && w1 > 0.0f) ? 2u : 1u};
                // If t is exactly 0 or 1, only one clip is active
                if (w1 <= 0.0f) {
                    best_result = {{i0, 0, 0}, {1.0f, 0.0f, 0.0f}, 1};
                } else if (w0 <= 0.0f) {
                    best_result = {{i1, 0, 0}, {1.0f, 0.0f, 0.0f}, 1};
                }
            }
        }
    }

    // Also check bare vertices (for degenerate cases or no triangles)
    for (uint32_t i = 0; i < static_cast<uint32_t>(points_.size()); ++i) {
        float dist2 = glm::dot(param - points_[i].position, param - points_[i].position);
        if (dist2 < best_dist2) {
            best_dist2 = dist2;
            best_result = {{i, 0, 0}, {1.0f, 0.0f, 0.0f}, 1};
        }
    }

    return best_result;
}

Result<BlendSpace2D> BlendSpace2D::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) return make_error("Failed to open .fjblend2D: " + path);

    auto j = nlohmann::json::parse(file);
    BlendSpace2D bs;
    bs.parameter_x_ = j.value("parameter_x", "");
    bs.parameter_y_ = j.value("parameter_y", "");
    bs.skeleton_path_ = j.value("skeleton", "");
    bs.damp_speed_ = j.value("damp_speed", 10.0f);

    if (j.contains("points") && j["points"].is_array()) {
        for (const auto& jp : j["points"]) {
            BlendSpacePoint2D pt;
            pt.clip = jp.value("clip", "");
            if (jp.contains("position") && jp["position"].is_array() && jp["position"].size() >= 2) {
                pt.position = {jp["position"][0].get<float>(), jp["position"][1].get<float>()};
            }
            bs.add(std::move(pt));
        }
    }

    bs.triangulate();

    FJELL_CORE_INFO("Loaded blend space 2D: {} ({} points, params='{}/{}', {} triangles)",
                    path, bs.size(), bs.parameter_x_, bs.parameter_y_,
                    static_cast<uint32_t>(bs.triangles_.size()));
    return bs;
}

Result<> BlendSpace2D::save(const std::string& path) const {
    nlohmann::json j;
    if (!skeleton_path_.empty()) j["skeleton"] = skeleton_path_;
    j["parameter_x"] = parameter_x_;
    j["parameter_y"] = parameter_y_;
    if (damp_speed_ != 10.0f) j["damp_speed"] = damp_speed_;
    j["points"] = nlohmann::json::array();

    for (const auto& pt : points_) {
        j["points"].push_back({
            {"clip", pt.clip},
            {"position", {pt.position.x, pt.position.y}}
        });
    }

    std::ofstream file(path);
    if (!file) return make_error("Failed to save .fjblend2D: " + path);
    file << j.dump(2);
    return {};
}

} // namespace fjell
