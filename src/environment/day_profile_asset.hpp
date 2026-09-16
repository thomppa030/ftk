#pragma once

#include "core/result.hpp"
#include "environment/track_set.hpp"

#include <string>
#include <string_view>

namespace fjell {

struct EnvironmentState;

// ── Day profile (.fjday) ─────────────────────────────────────────────────
//
// The look of a day, keyed over 24 hours and reusable across scenes: what
// colour and how strong the sun is at each hour, the ambient, the built-in
// gradient sky, the fog, an exposure bias, the moon and stars. Each is a
// track; a track the file leaves out leaves the authored value alone, so a
// scene with no profile and a profile with no tracks look the same.
//
// Custom tracks, any name the built-in list does not claim, become sky
// shader uniforms of that name and are readable by scripts.

/// The tracks the engine reads by name. Colour tracks are gradients, the
/// rest are curves. Multiply tracks scale what the light and fog nodes
/// author; override tracks replace it.
namespace day_track {
inline constexpr std::string_view SUN_COLOR = "sun_color";               // color, × light colour
inline constexpr std::string_view SUN_INTENSITY = "sun_intensity";       // float, × light intensity
inline constexpr std::string_view MOON_COLOR = "moon_color";             // color
inline constexpr std::string_view MOON_INTENSITY = "moon_intensity";     // float
inline constexpr std::string_view AMBIENT_COLOR = "ambient_color";       // color
inline constexpr std::string_view AMBIENT_INTENSITY = "ambient_intensity"; // float
inline constexpr std::string_view SKY_TOP = "sky_top";                   // color, gradient sky zenith
inline constexpr std::string_view SKY_BOTTOM = "sky_bottom";             // color, gradient sky horizon
inline constexpr std::string_view FOG_COLOR = "fog_color";               // color
inline constexpr std::string_view FOG_DENSITY = "fog_density";           // float, × fog density
inline constexpr std::string_view EXPOSURE_BIAS = "exposure_bias";       // float, EV added to the tonemap
inline constexpr std::string_view STAR_INTENSITY = "star_intensity";     // float, sky built-in STARS

/// Every built-in track name, for editors that offer them.
[[nodiscard]] const std::vector<std::string_view>& all();
/// The kind a built-in track has; Float for a custom name.
[[nodiscard]] TrackKind kind_of(std::string_view name);
[[nodiscard]] bool is_builtin(std::string_view name);
} // namespace day_track

struct DayProfileAsset {
    /// Display name, for the inspector. Defaults to the file stem.
    std::string name;
    /// Keyed in hours, periodic: 24 leads back into 0.
    TrackSet tracks;

    DayProfileAsset() {
        tracks.length = 24.0f;
        tracks.wrap = true;
    }

    /// Fold the profile at `hour` into a resolved environment. The sun and
    /// moon bodies must already carry their authored colour and intensity;
    /// the horizon fade is applied by the caller afterwards.
    void apply(float hour, EnvironmentState& state) const;
};

[[nodiscard]] Result<DayProfileAsset> load_day_profile_file(const std::string& path);
[[nodiscard]] Result<> save_day_profile_file(const std::string& path, const DayProfileAsset& asset);

} // namespace fjell
