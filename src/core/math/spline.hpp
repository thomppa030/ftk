#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fjell {

/// Per-point interpolation mode. A spline can mix modes across its points —
/// a single spline can contain smooth curves, sharp corners, and straight
/// segments without being split.
///
/// The mode on point N describes the segment leaving N (to N+1), with the
/// exception of tangent authoring which involves both arrive and leave.
enum class SplineInterp : uint8_t {
    Linear,       ///< straight line to next point, tangents ignored
    CurveAuto,    ///< tangents auto-computed from neighbors (Catmull-Rom-like)
    CurveUser,    ///< tangents authored, arrive == leave (smooth)
    CurveBreak,   ///< tangents authored, arrive and leave independent (corner)
    Constant,     ///< no interpolation — holds value until next point
};

/// Single control point on a spline.
///
/// `input_key` is the parameter coordinate for this point. Points must be kept
/// monotonically increasing in `input_key`. For a time-based spline this is
/// seconds; for a path it's typically an arbitrary float the author doesn't
/// see directly.
///
/// `arrive_tangent` and `leave_tangent` are derivatives `dValue/dInputKey` at
/// this point. They are consulted only in `CurveUser` and `CurveBreak` modes
/// (and overwritten by `auto_set_tangents()` for `CurveAuto` points).
template <typename T>
struct SplinePoint {
    float        input_key{0.0f};
    T            value{};
    T            arrive_tangent{};
    T            leave_tangent{};
    SplineInterp interp_mode{SplineInterp::CurveAuto};
};

/// Cubic-Hermite spline with per-point interpolation modes. Generic over
/// `T ∈ {float, vec2, vec3}`. Modeled after Unreal's `FInterpCurve<T>`.
///
/// Workflow:
///   1. add_point(key, value) — defaults to CurveAuto
///   2. auto_set_tangents()   — recomputes tangents for every CurveAuto point
///   3. evaluate(key)         — sample the curve
///
/// For uniform-speed traversal, call `build_reparam_table()` once after the
/// curve is edited, then use `evaluate_at_distance()` / `total_length()`.
template <typename T>
class Spline {
public:
    std::vector<SplinePoint<T>> points;
    bool closed{false};

    // ── Evaluation ────────────────────────────────────────────────────────
    [[nodiscard]] T evaluate(float input_key) const;
    [[nodiscard]] T tangent(float input_key) const;

    // ── Editing ───────────────────────────────────────────────────────────
    /// Insert a point, keeping `input_key` monotonic. Returns the new index.
    size_t add_point(float input_key, const T& value,
                     SplineInterp mode = SplineInterp::CurveAuto);
    void   remove_point(size_t index);
    void   set_point_value(size_t index, const T& value);
    void   set_point_interp(size_t index, SplineInterp mode);

    /// Recompute tangents for all CurveAuto points. Call after editing values.
    /// `tension` in [0, 1] — 0 is Catmull-Rom, 1 collapses to linear.
    void auto_set_tangents(float tension = 0.0f);

    // ── Arc-length reparameterization ─────────────────────────────────────
    /// Build the reparam table. O(samples) work, no-op on empty splines.
    void  build_reparam_table(uint32_t samples = 256);
    [[nodiscard]] float total_length() const;
    [[nodiscard]] float input_key_at_distance(float distance) const;
    [[nodiscard]] T     evaluate_at_distance(float distance) const;

    // ── Introspection ─────────────────────────────────────────────────────
    [[nodiscard]] bool  empty() const { return points.empty(); }
    [[nodiscard]] float min_key() const { return points.empty() ? 0.0f : points.front().input_key; }
    /// Max evaluatable input key. For closed splines this extends past the
    /// last point by one closing segment's span so queries can cover the
    /// loop-back to points.front().
    [[nodiscard]] float max_key() const;
    /// Input-key span of the virtual closing segment (back -> front) when
    /// `closed` is true. Returns 0 when the spline isn't closed or has < 2
    /// points.
    [[nodiscard]] float closing_span() const;

private:
    std::vector<float> reparam_keys_;       ///< input_key at each reparam sample
    std::vector<float> reparam_distances_;  ///< cumulative arc length at each sample

    /// Locate segment [i, i+1] containing `input_key`. Returns i, clamping at
    /// the ends. `local_t` is set to the [0, 1] parameter within the segment.
    size_t find_segment(float input_key, float& local_t) const;
};

// Explicit instantiations provided in spline.cpp for:
extern template class Spline<float>;
extern template class Spline<glm::vec2>;
extern template class Spline<glm::vec3>;

} // namespace fjell
