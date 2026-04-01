#pragma once

#include "core/result.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

// ── BlendSpace1D ────────────────────────────────────────────────────────

// A point in a 1D blend space: an animation clip at a parameter position.
struct BlendSpacePoint {
    std::string clip;   // path to .fjanim
    float position;     // parameter value (e.g. 0 = idle, 0.5 = walk, 1.0 = run)
};

// Result of evaluating a BlendSpace1D: the two neighboring clips and their weights.
struct BlendResult {
    uint32_t index_a{0};
    uint32_t index_b{0};
    float weight_a{1.0f};  // weight for clip A
    float weight_b{0.0f};  // weight for clip B
};

// 1D blend space. Sorted list of clip-position points. Given a parameter value,
// finds the two neighboring points and computes blend weights via linear interpolation.
class BlendSpace1D {
public:
    void add(BlendSpacePoint point);
    void clear() { points_.clear(); }

    [[nodiscard]] BlendResult evaluate(float param) const;

    [[nodiscard]] static Result<BlendSpace1D> load(const std::string& path);
    [[nodiscard]] Result<> save(const std::string& path) const;

    [[nodiscard]] const std::string& parameter() const { return parameter_; }
    void set_parameter(const std::string& name) { parameter_ = name; }

    [[nodiscard]] const std::string& skeleton_path() const { return skeleton_path_; }
    void set_skeleton_path(const std::string& path) { skeleton_path_ = path; }

    [[nodiscard]] float damp_speed() const { return damp_speed_; }
    void set_damp_speed(float speed) { damp_speed_ = speed; }

    [[nodiscard]] const std::vector<BlendSpacePoint>& points() const { return points_; }
    [[nodiscard]] bool empty() const { return points_.empty(); }
    [[nodiscard]] uint32_t size() const { return static_cast<uint32_t>(points_.size()); }

private:
    std::string parameter_;                // parameter name (e.g. "speed")
    std::string skeleton_path_;            // relative path to .fjskel
    float damp_speed_{10.0f};              // parameter smoothing speed (higher = faster, 0 = instant)
    std::vector<BlendSpacePoint> points_;  // sorted by position
};

// ── BlendSpace2D ────────────────────────────────────────────────────────

struct BlendSpacePoint2D {
    std::string clip;    // path to .fjanim
    glm::vec2 position;  // 2D parameter position
};

// Result of evaluating a BlendSpace2D: up to 3 contributing clips via barycentric weights.
struct BlendResult2D {
    uint32_t indices[3]{0, 0, 0};
    float weights[3]{1.0f, 0.0f, 0.0f};
    uint32_t count{1};  // number of active clips (1, 2, or 3)
};

// 2D blend space. Points placed in 2D parameter space, triangulated via Delaunay.
// Evaluates via barycentric interpolation within the containing triangle.
class BlendSpace2D {
public:
    void add(BlendSpacePoint2D point);
    void clear() { points_.clear(); triangles_.clear(); }
    void triangulate();

    [[nodiscard]] BlendResult2D evaluate(glm::vec2 param) const;

    [[nodiscard]] static Result<BlendSpace2D> load(const std::string& path);
    [[nodiscard]] Result<> save(const std::string& path) const;

    [[nodiscard]] const std::string& parameter_x() const { return parameter_x_; }
    [[nodiscard]] const std::string& parameter_y() const { return parameter_y_; }
    void set_parameters(const std::string& x, const std::string& y) {
        parameter_x_ = x;
        parameter_y_ = y;
    }

    [[nodiscard]] const std::string& skeleton_path() const { return skeleton_path_; }
    void set_skeleton_path(const std::string& path) { skeleton_path_ = path; }

    [[nodiscard]] float damp_speed() const { return damp_speed_; }
    void set_damp_speed(float speed) { damp_speed_ = speed; }

    [[nodiscard]] const std::vector<BlendSpacePoint2D>& points() const { return points_; }
    [[nodiscard]] bool empty() const { return points_.empty(); }
    [[nodiscard]] uint32_t size() const { return static_cast<uint32_t>(points_.size()); }

    struct Triangle { uint32_t a, b, c; };
    [[nodiscard]] const std::vector<Triangle>& triangles() const { return triangles_; }

private:
    std::string parameter_x_;
    std::string parameter_y_;
    std::string skeleton_path_;
    float damp_speed_{10.0f};              // parameter smoothing speed (higher = faster, 0 = instant)
    std::vector<BlendSpacePoint2D> points_;
    std::vector<Triangle> triangles_;
};

} // namespace fjell
