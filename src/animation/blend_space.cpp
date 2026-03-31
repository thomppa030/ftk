#include "animation/blend_space.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

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

BlendSpace1D BlendSpace1D::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Failed to open .fjblend1D: " + path);

    auto j = nlohmann::json::parse(file);
    BlendSpace1D bs;
    bs.parameter_ = j.value("parameter", "");
    bs.skeleton_path_ = j.value("skeleton", "");

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

void BlendSpace1D::save(const std::string& path) const {
    nlohmann::json j;
    if (!skeleton_path_.empty()) j["skeleton"] = skeleton_path_;
    j["parameter"] = parameter_;
    j["points"] = nlohmann::json::array();

    for (const auto& pt : points_) {
        j["points"].push_back({{"clip", pt.clip}, {"position", pt.position}});
    }

    std::ofstream file(path);
    if (!file) throw std::runtime_error("Failed to save .fjblend1D: " + path);
    file << j.dump(2);
}

// ── BlendSpace2D ────────────────────────────────────────────────────────

void BlendSpace2D::add(BlendSpacePoint2D point) {
    points_.push_back(std::move(point));
}

void BlendSpace2D::triangulate() {
    triangles_.clear();
    uint32_t n = static_cast<uint32_t>(points_.size());
    if (n < 3) return;

    // Simple ear-clipping / fan triangulation for small point sets.
    // For production, Bowyer-Watson Delaunay would be better, but blend spaces
    // typically have 5-15 points so a simpler approach works.

    // Collect 2D positions
    struct IdxPt { uint32_t idx; glm::vec2 pos; };
    std::vector<IdxPt> pts(n);
    for (uint32_t i = 0; i < n; ++i) {
        pts[i] = {i, points_[i].position};
    }

    // Sort by angle from centroid for a convex-hull-like fan
    glm::vec2 centroid{0.0f};
    for (const auto& p : pts) centroid += p.pos;
    centroid /= static_cast<float>(n);

    std::sort(pts.begin(), pts.end(), [&](const IdxPt& a, const IdxPt& b) {
        return std::atan2(a.pos.y - centroid.y, a.pos.x - centroid.x)
             < std::atan2(b.pos.y - centroid.y, b.pos.x - centroid.x);
    });

    // Fan triangulation from centroid ordering
    for (uint32_t i = 0; i < n - 2; ++i) {
        triangles_.push_back({pts[0].idx, pts[i + 1].idx, pts[i + 2].idx});
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

BlendSpace2D BlendSpace2D::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Failed to open .fjblend2D: " + path);

    auto j = nlohmann::json::parse(file);
    BlendSpace2D bs;
    bs.parameter_x_ = j.value("parameter_x", "");
    bs.parameter_y_ = j.value("parameter_y", "");
    bs.skeleton_path_ = j.value("skeleton", "");

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

void BlendSpace2D::save(const std::string& path) const {
    nlohmann::json j;
    if (!skeleton_path_.empty()) j["skeleton"] = skeleton_path_;
    j["parameter_x"] = parameter_x_;
    j["parameter_y"] = parameter_y_;
    j["points"] = nlohmann::json::array();

    for (const auto& pt : points_) {
        j["points"].push_back({
            {"clip", pt.clip},
            {"position", {pt.position.x, pt.position.y}}
        });
    }

    std::ofstream file(path);
    if (!file) throw std::runtime_error("Failed to save .fjblend2D: " + path);
    file << j.dump(2);
}

} // namespace fjell
