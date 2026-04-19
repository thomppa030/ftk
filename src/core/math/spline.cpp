#include "core/math/spline.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace fjell {

namespace {

template <typename T>
T zero() {
    return T{0};
}

template <>
glm::vec2 zero() { return glm::vec2{0.0f}; }
template <>
glm::vec3 zero() { return glm::vec3{0.0f}; }

float length_of(float v) { return std::abs(v); }
float length_of(const glm::vec2& v) { return glm::length(v); }
float length_of(const glm::vec3& v) { return glm::length(v); }

/// Cubic Hermite basis. h00*p0 + h10*m0 + h01*p1 + h11*m1, with tangents
/// scaled by segment length `dx` (tangents are stored as dValue/dInputKey).
template <typename T>
T hermite(const T& p0, const T& m0, const T& p1, const T& m1, float t, float dx) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 =  2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 =         t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 =         t3 -        t2;
    return h00 * p0 + (h10 * dx) * m0 + h01 * p1 + (h11 * dx) * m1;
}

/// Hermite derivative w.r.t. t, then scaled by 1/dx to return dValue/dInputKey.
template <typename T>
T hermite_derivative(const T& p0, const T& m0, const T& p1, const T& m1, float t, float dx) {
    const float t2 = t * t;
    const float dh00 =  6.0f * t2 -  6.0f * t;
    const float dh10 =  3.0f * t2 -  4.0f * t + 1.0f;
    const float dh01 = -6.0f * t2 +  6.0f * t;
    const float dh11 =  3.0f * t2 -  2.0f * t;
    const T d_dt = dh00 * p0 + (dh10 * dx) * m0 + dh01 * p1 + (dh11 * dx) * m1;
    return d_dt * (1.0f / dx);
}

} // namespace

template <typename T>
float Spline<T>::closing_span() const {
    const size_t n = points.size();
    if (!closed || n < 2) return 0.0F;
    // Use the final authored segment's span so the loop-back rate matches
    // the surrounding curve. Fall back to 1.0 if somehow zero.
    const float span = points[n - 1].input_key - points[n - 2].input_key;
    return span > 0.0F ? span : 1.0F;
}

template <typename T>
float Spline<T>::max_key() const {
    if (points.empty()) return 0.0F;
    return points.back().input_key + closing_span();
}

template <typename T>
size_t Spline<T>::find_segment(float input_key, float& local_t) const {
    const size_t n = points.size();
    if (n < 2) {
        local_t = 0.0f;
        return 0;
    }
    if (input_key <= points.front().input_key) {
        local_t = 0.0f;
        return 0;
    }
    // Closed: queries past the last point evaluate the virtual closing
    // segment. Signal this by returning index n - 1 with local_t in [0,1];
    // the caller pairs points[n-1] with points[0] in that case.
    const float span = closing_span();
    if (closed && span > 0.0F && input_key >= points.back().input_key) {
        const float over = input_key - points.back().input_key;
        local_t = std::min(over / span, 1.0F);
        return n - 1;
    }
    if (input_key >= points.back().input_key) {
        local_t = 1.0f;
        return n - 2;
    }
    size_t lo = 0;
    size_t hi = n - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (points[mid].input_key <= input_key) lo = mid;
        else                                    hi = mid;
    }
    const float dx = points[hi].input_key - points[lo].input_key;
    local_t = dx > 0.0f ? (input_key - points[lo].input_key) / dx : 0.0f;
    return lo;
}

template <typename T>
T Spline<T>::evaluate(float input_key) const {
    if (points.empty()) return zero<T>();
    if (points.size() == 1) return points.front().value;

    float t = 0.0f;
    const size_t i = find_segment(input_key, t);
    const size_t n = points.size();
    // Closing segment: find_segment returns i == n-1 only when closed.
    const bool closing = (i == n - 1);
    const SplinePoint<T>& a = points[i];
    const SplinePoint<T>& b = closing ? points[0] : points[i + 1];

    switch (a.interp_mode) {
        case SplineInterp::Constant:
            // Hold a.value until we land exactly at b's key, then snap.
            return t >= 1.0F ? b.value : a.value;
        case SplineInterp::Linear:
            return a.value + (b.value - a.value) * t;
        case SplineInterp::CurveAuto:
        case SplineInterp::CurveUser:
        case SplineInterp::CurveBreak: {
            const float dx = closing ? closing_span() : (b.input_key - a.input_key);
            return hermite(a.value, a.leave_tangent, b.value, b.arrive_tangent, t, dx);
        }
    }
    return a.value;
}

template <typename T>
T Spline<T>::tangent(float input_key) const {
    if (points.size() < 2) return zero<T>();

    float t = 0.0f;
    const size_t i = find_segment(input_key, t);
    const size_t n = points.size();
    const bool closing = (i == n - 1);
    const SplinePoint<T>& a = points[i];
    const SplinePoint<T>& b = closing ? points[0] : points[i + 1];
    const float dx = closing ? closing_span() : (b.input_key - a.input_key);

    switch (a.interp_mode) {
        case SplineInterp::Constant:
            return zero<T>();
        case SplineInterp::Linear:
            return dx > 0.0f ? (b.value - a.value) * (1.0f / dx) : zero<T>();
        case SplineInterp::CurveAuto:
        case SplineInterp::CurveUser:
        case SplineInterp::CurveBreak:
            return hermite_derivative(a.value, a.leave_tangent, b.value, b.arrive_tangent, t, dx);
    }
    return zero<T>();
}

template <typename T>
size_t Spline<T>::add_point(float input_key, const T& value, SplineInterp mode) {
    SplinePoint<T> p;
    p.input_key   = input_key;
    p.value       = value;
    p.interp_mode = mode;
    auto it = std::upper_bound(points.begin(), points.end(), input_key,
        [](float key, const SplinePoint<T>& pt) { return key < pt.input_key; });
    const size_t idx = static_cast<size_t>(it - points.begin());
    points.insert(it, p);
    return idx;
}

template <typename T>
void Spline<T>::remove_point(size_t index) {
    if (index < points.size()) points.erase(points.begin() + static_cast<long>(index));
}

template <typename T>
void Spline<T>::set_point_value(size_t index, const T& value) {
    if (index < points.size()) points[index].value = value;
}

template <typename T>
void Spline<T>::set_point_interp(size_t index, SplineInterp mode) {
    if (index < points.size()) points[index].interp_mode = mode;
}

template <typename T>
void Spline<T>::auto_set_tangents(float tension) {
    const float scale = 0.5f * (1.0f - tension);
    const size_t n = points.size();
    if (n < 2) return;

    for (size_t i = 0; i < n; ++i) {
        if (points[i].interp_mode != SplineInterp::CurveAuto) continue;

        // Catmull-Rom: tangent at i = (value[i+1] - value[i-1]) / (key[i+1] - key[i-1])
        // Endpoints (unless closed) use the one-sided difference.
        T   prev_v  = points[i].value;
        T   next_v  = points[i].value;
        float prev_k = points[i].input_key;
        float next_k = points[i].input_key;

        if (i == 0) {
            if (closed && n >= 2) {
                // wrap: previous is last point, shifted back by the curve span
                const float span = points.back().input_key - points.front().input_key;
                prev_v = points[n - 1].value;
                prev_k = points[n - 1].input_key - span;
            } else {
                prev_v = points[i].value;
                prev_k = points[i].input_key;
            }
        } else {
            prev_v = points[i - 1].value;
            prev_k = points[i - 1].input_key;
        }

        if (i + 1 >= n) {
            if (closed && n >= 2) {
                const float span = points.back().input_key - points.front().input_key;
                next_v = points[0].value;
                next_k = points[0].input_key + span;
            } else {
                next_v = points[i].value;
                next_k = points[i].input_key;
            }
        } else {
            next_v = points[i + 1].value;
            next_k = points[i + 1].input_key;
        }

        const float dk = next_k - prev_k;
        const T tan = dk > 0.0f ? (next_v - prev_v) * (scale * 2.0f / dk)
                                 : zero<T>();
        points[i].arrive_tangent = tan;
        points[i].leave_tangent  = tan;
    }
}

template <typename T>
void Spline<T>::build_reparam_table(uint32_t samples) {
    reparam_keys_.clear();
    reparam_distances_.clear();
    if (points.size() < 2 || samples < 2) return;

    reparam_keys_.reserve(samples);
    reparam_distances_.reserve(samples);

    const float k0 = min_key();
    const float k1 = max_key();  // covers the closing segment when closed
    const float step = (k1 - k0) / static_cast<float>(samples - 1);

    T   prev_v = evaluate(k0);
    float accum = 0.0f;
    reparam_keys_.push_back(k0);
    reparam_distances_.push_back(0.0f);

    for (uint32_t i = 1; i < samples; ++i) {
        const float k = k0 + step * static_cast<float>(i);
        const T v = evaluate(k);
        accum += length_of(v - prev_v);
        reparam_keys_.push_back(k);
        reparam_distances_.push_back(accum);
        prev_v = v;
    }
}

template <typename T>
float Spline<T>::total_length() const {
    return reparam_distances_.empty() ? 0.0f : reparam_distances_.back();
}

template <typename T>
float Spline<T>::input_key_at_distance(float distance) const {
    if (reparam_distances_.size() < 2) return min_key();
    if (distance <= 0.0f)                return reparam_keys_.front();
    if (distance >= total_length())      return reparam_keys_.back();

    // Binary search for the distance bracket.
    auto it = std::upper_bound(reparam_distances_.begin(), reparam_distances_.end(), distance);
    const size_t hi = static_cast<size_t>(it - reparam_distances_.begin());
    const size_t lo = hi - 1;
    const float d0 = reparam_distances_[lo];
    const float d1 = reparam_distances_[hi];
    const float seg = d1 - d0;
    const float t = seg > 0.0f ? (distance - d0) / seg : 0.0f;
    return reparam_keys_[lo] + t * (reparam_keys_[hi] - reparam_keys_[lo]);
}

template <typename T>
T Spline<T>::evaluate_at_distance(float distance) const {
    return evaluate(input_key_at_distance(distance));
}

// ── Explicit instantiations ───────────────────────────────────────────────
template class Spline<float>;
template class Spline<glm::vec2>;
template class Spline<glm::vec3>;

} // namespace fjell
