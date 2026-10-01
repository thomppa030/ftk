#pragma once

#include "environment/color_gradient.hpp"

#include "core/named_value.hpp"
#include "ftk/math/curve.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

// ── Tracks ───────────────────────────────────────────────────────────────
//
// The one authoring primitive behind the environment's keyed assets: a day
// profile keys its tracks over 24 hours, a weather event over its own length,
// and both are edited by the same widget. A track is a float Curve or a
// ColorGradient with a name, a mode that says how it meets the value it
// shapes, and keys in the set's domain units (hours, seconds).
//
// Curves and gradients keep their keys normalized to 0..1 internally; the
// set's `length` is the domain, applied when a value is read or a key is
// written through the set, and in the file.

enum class TrackKind : uint8_t { Float, Color };

/// How a track's value meets the value it drives: a multiplier keeps what
/// was authored underneath and scales it, an override replaces it.
enum class TrackMode : uint8_t { Override, Multiply };

struct Track {
    std::string name;
    TrackKind kind{TrackKind::Float};
    TrackMode mode{TrackMode::Override};
    ftk::Curve curve;                 // Float tracks
    ColorGradient gradient;      // Color tracks
};

struct TrackSet {
    std::vector<Track> tracks;
    /// Length of the domain the keys are authored in: 24 for hours, an
    /// event's seconds, 1 for already normalized time.
    float length{1.0f};
    /// Periodic domain: the last key leads back into the first (a day).
    bool wrap{false};

    [[nodiscard]] Track* find(std::string_view name);
    [[nodiscard]] const Track* find(std::string_view name) const;
    [[nodiscard]] bool has(std::string_view name) const { return find(name) != nullptr; }

    /// Add a track, replacing one of the same name.
    Track& add(std::string name, TrackKind kind, TrackMode mode);
    void remove(std::string_view name);

    /// The value of a track at `x` in domain units, or nothing when the set
    /// has no track of that name or kind.
    [[nodiscard]] std::optional<float> value_of(std::string_view name, float x) const;
    [[nodiscard]] std::optional<glm::vec3> color_of(std::string_view name, float x) const;

    /// Fold a track's value into `target` by its mode; leaves `target` alone
    /// when the track does not exist.
    void apply(std::string_view name, float x, float& target) const;
    void apply(std::string_view name, float x, glm::vec3& target) const;

    void to_json(nlohmann::json& j) const;
    void from_json(const nlohmann::json& j);
};

/// A Curve read periodically: outside its first and last key the gap wraps
/// around the domain instead of holding the end values.
[[nodiscard]] float evaluate_periodic(const ftk::Curve& curve, float t);

/// Fold a track's value into a named uniform value by the track's mode,
/// faded in by `weight` (1 applies it whole, as a day profile does; a weather
/// event passes how present it is). A multiply scales what is there and an
/// override moves toward the value. An sRGB colour is scaled and moved in
/// linear light, which is what scaling a node's own colour field did before
/// the value lived in a material.
///
/// A float folds into the first component; a colour into the first three of
/// a value that has them. A colour track meeting a float, or the other way
/// round, is a mismatch and leaves the value alone.
void fold_track(NamedValue& target, TrackMode mode, float value, float weight = 1.0f);
void fold_track(NamedValue& target, TrackMode mode, const glm::vec3& value, float weight = 1.0f);

} // namespace fjell
