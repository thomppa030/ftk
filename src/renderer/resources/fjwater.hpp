#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

/// How many wave trains a water surface may sum. The shader unrolls this
/// many iterations and the CPU evaluator matches, so it is a compile-time
/// bound on both sides rather than a soft limit.
inline constexpr std::uint32_t MAX_WATER_WAVES = 16;

/// One Gerstner wave train.
///
/// Gerstner (rather than a plain sine) because it displaces horizontally as
/// well as vertically, bunching vertices toward the crest. That pinches peaks
/// and broadens troughs, which is the silhouette stylized water reads as; a
/// sum of sines gives rolling hills instead. The horizontal term is also what
/// makes crest steepness computable, which is where whitecap foam comes from.
struct WaterWave {
    /// Direction of travel across the ground plane, world XZ. Normalized on
    /// load; a zero direction disables the train.
    glm::vec2 direction{1.0f, 0.0f};

    /// Crest-to-crest distance in metres. Drives frequency (2*pi/wavelength)
    /// and, with speed, how fast the train moves.
    float wavelength{20.0f};

    /// Crest height above the still surface, in metres.
    float amplitude{0.5f};

    /// How far the wave pinches its crest, 0 (a sine) to 1 (the sharpest
    /// this train can go before it folds over itself). Kept as a 0..1 dial
    /// rather than the raw Q of the Gerstner formula, which depends on
    /// amplitude and frequency and loops back on itself past the limit.
    float steepness{0.5f};

    /// Multiplier on the wave's natural deep-water speed, so 1 travels at the
    /// speed its wavelength implies. Slower reads as heavier water.
    float speed{1.0f};
};

/// The settings a generator fills a wave list from.
///
/// Authoring convenience only: nothing at runtime reads these, and the wave
/// list stands alone once written. They are saved so the editor can put its
/// sliders back where they were and generate again.
struct WaterWaveGenerator {
    /// Direction the wind blows across the ground plane, world XZ.
    glm::vec2 wind_direction{1.0f, 0.0f};

    /// Amplitude of the largest train, in metres. Every other train falls off
    /// from here.
    float swell{1.0f};

    /// Steepness given to the generated trains, 0..1.
    float chop{0.7f};

    /// Wavelength of the largest train, in metres.
    float longest_wavelength{30.0f};

    /// How many trains to generate, up to MAX_WATER_WAVES.
    std::uint32_t wave_count{6};

    /// Half-angle the generated directions fan out from the wind, in degrees.
    /// 0 makes every train travel the same way, which reads as a canal rather
    /// than open water.
    float spread_degrees{40.0f};

    /// How quickly the trains shrink down the list, as the ratio between one
    /// wavelength and the next.
    ///
    /// This is what separates a wind sea from a swell. Near 0.5 each train is
    /// half the last, so the long ones are quickly buried under chop and the
    /// surface reads as locally whipped up. Near 0.85 the trains stay close in
    /// size, the long ones keep the surface, and what arrives is a procession
    /// of broad ridges — the rolling swell of water that has travelled from a
    /// storm somewhere else and sorted itself on the way.
    float wavelength_falloff{0.62f};

    /// How quickly the trains lose height down the list.
    ///
    /// Held separate from the wavelength falloff because the two say different
    /// things: a swell is long *and* smooth, so its short trains want to fade
    /// faster than its long ones shorten.
    float amplitude_falloff{0.68f};
};

/// How a sea answers the wind the weather system is blowing.
///
/// The authored wave list is the sea at the reference wind, not the sea in a
/// dead calm: an artist tunes a `.fjwater` until it looks right, and that is
/// the look they signed off on. Wind then moves it either side of that.
/// Authoring the calm instead would mean tuning a sea nobody ever sees.
///
/// Nothing here is applied unless a scene has weather driving it. A
/// `.fjwater` opened in the editor, or placed in a scene with no wind, is
/// exactly the field that was authored.
struct WaterWindResponse {
    /// Whether the weather's wind reaches this water at all. Off for a pool,
    /// a canal, anything indoors, and for a sea an artist wants to hold still
    /// while they tune it.
    bool enabled{false};

    /// The wind speed the authored field describes, in m/s. Wind at this
    /// speed changes nothing.
    ///
    /// 8 m/s is a fresh breeze — whitecaps forming, the condition most open
    /// water is authored at, and the middle of the range a sea spends its
    /// time in.
    float reference_speed{8.0f};

    /// How much wind speed drives wave height, as the amplitude multiplier at
    /// twice the reference wind.
    ///
    /// Real wave height grows steeply with wind — roughly with its square in
    /// a fully developed sea — but a sea that quadruples in a gust reads as a
    /// bug rather than as weather, so this is a straight ramp through the
    /// reference point and an artist's number rather than an oceanographer's.
    /// 1 leaves height alone and lets wind steer direction only.
    float amplitude_response{1.6f};

    /// How much of the fold budget a strong wind may additionally spend, at
    /// twice the reference wind.
    ///
    /// Wind does not only raise a sea, it sharpens it: the same swell under a
    /// gale is pinched where under a breeze it is round. This multiplies
    /// `steepness_headroom`, so a sea authored near its fold ceiling will
    /// fold in a storm — which is the intent, and why it is a separate dial
    /// from height rather than folded into it.
    float steepness_response{1.25f};

    /// How far the wind turns the trains, 0 leaving every train on its
    /// authored heading and 1 turning the shortest all the way onto the wind.
    ///
    /// Only the short trains turn. A long swell was raised by a storm
    /// somewhere else and holds its direction for days after the local wind
    /// has changed; the chop riding on it answers the wind it is in. The ramp
    /// runs by wavelength between the longest and shortest train, so a sea
    /// keeps its swell while the wind combs the surface.
    float direction_response{0.7f};
};

/// A `.fjwater`: the wave field of a body of water.
///
/// The waves are the whole runtime asset. Colour, foam look, refraction and
/// the rest of the surface's appearance live in its material, because the
/// split that matters is whether the CPU needs to know: buoyancy, swimming
/// and editor gizmos all ask where the surface is, and none of them care what
/// colour it is.
///
/// Both the shader and the CPU evaluate this same list with the same formula,
/// so a boat sits on the crest the player can see. That only holds while the
/// two implementations agree, which is what test_fjwater.cpp pins down.
struct FjWaterData {
    /// The trains to sum, in no particular order. Beyond MAX_WATER_WAVES the
    /// tail is dropped on load.
    std::vector<WaterWave> waves;

    /// What the generator last produced this list from. Ignored at runtime.
    WaterWaveGenerator generator;

    /// Multiplies every train's amplitude, for scaling a whole sea up or down
    /// without touching each wave. The place to drive "calm to storm" from.
    float amplitude_scale{1.0f};

    /// Multiplies every train's speed, for slowing or hastening the whole sea
    /// at once.
    float speed_scale{1.0f};

    /// How far past a single train's fold limit the whole field may be
    /// pushed, as a multiplier on every train's steepness.
    ///
    /// At 1 each train's `steepness` means exactly what it says: 1.0 is that
    /// train at its own fold limit, and a lone train can never fold. Trains
    /// sum, though, and the surface folds on the sum, so a field of several
    /// steep trains can still fold where their crests meet — that is not
    /// prevented here, it is measured. The editor samples the surface and
    /// reports what percentage of it folds at the current settings.
    ///
    /// Raising it is a deliberate trade: sharper crests everywhere in exchange
    /// for the sharpest of them turning inside out, which looks like dark flat
    /// facets sitting in the foam. Some folding at distance is often worth it.
    ///
    /// 0.30 is the default because a field is many trains and they sum: on the
    /// generator's own six-train sea, measured over a 80 m square across eight
    /// seconds, nothing folds at 0.35 and 14% of the surface folds at 1.0. So
    /// the default sits just under the measured ceiling for the sea the
    /// generator makes, and a sea of fewer or gentler trains can go far higher.
    /// The editor measures yours rather than guessing from this number.
    float steepness_headroom{0.30f};

    /// How the sea answers the weather's wind.
    WaterWindResponse wind;

    /// The `.fjmat` the water editor draws this field through, as an asset
    /// reference. Empty previews with the engine's own water shader.
    ///
    /// Authoring convenience, like `generator`: nothing at runtime reads it,
    /// and a placed body of water takes its material from its own component.
    /// It lives here because it belongs to the field being tuned — crest
    /// steepness and foam read differently under every material, so the one
    /// a sea was judged against is part of how to come back to it.
    std::string preview_material;
};

/// Fill a wave list from generator settings: wavelengths halving from the
/// longest, amplitudes falling with them, directions fanned across the wind,
/// and speeds drifting so the trains do not beat in lockstep.
///
/// Deterministic — the same settings always give the same list, so pressing
/// generate twice is not a reroll.
[[nodiscard]] std::vector<WaterWave> generate_water_waves(const WaterWaveGenerator& settings);

} // namespace fjell
