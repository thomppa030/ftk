#pragma once

#include <glm/glm.hpp>

#include <cstdint>
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
};

/// Fill a wave list from generator settings: wavelengths halving from the
/// longest, amplitudes falling with them, directions fanned across the wind,
/// and speeds drifting so the trains do not beat in lockstep.
///
/// Deterministic — the same settings always give the same list, so pressing
/// generate twice is not a reroll.
[[nodiscard]] std::vector<WaterWave> generate_water_waves(const WaterWaveGenerator& settings);

} // namespace fjell
