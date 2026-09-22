#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fjell {

/// How many wave trains one Gerstner contributor may sum. The shader unrolls
/// this many iterations and the CPU evaluator matches, so it is a compile-time
/// bound on both sides rather than a soft limit.
inline constexpr std::uint32_t MAX_WATER_WAVES = 16;

/// How many Gerstner contributors a water surface may sum, a bound on both
/// sides for the same reason. Few, because a contributor is a whole sea's
/// worth of trains: a swell and the chop riding on it are two.
inline constexpr std::uint32_t MAX_GERSTNER_CONTRIBUTORS = 4;

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

/// Art-directed water: a list of Gerstner trains an author can reason about
/// one at a time — "the swell comes from the north-west, big and slow".
///
/// Closed-form, so the shader and the CPU evaluate it with the same formula
/// and a boat sits on the crest the player can see. That only holds while the
/// two implementations agree, which is what test_fjwater.cpp pins down.
struct GerstnerContributor {
    /// The trains to sum, in no particular order. Beyond MAX_WATER_WAVES the
    /// tail is dropped on load.
    std::vector<WaterWave> waves;

    /// What the generator last produced this list from. Ignored at runtime.
    WaterWaveGenerator generator;

    /// Multiplies every train's amplitude, for scaling these trains up or
    /// down without touching each wave. The place to drive "calm to storm"
    /// from.
    float amplitude_scale{1.0f};

    /// Multiplies every train's speed, for slowing or hastening these trains
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
    /// Raising it buys sharper crests everywhere and makes the sharpest of them
    /// fold, which is a wave breaking rather than a defect: a crest that
    /// outruns its own base and throws over is what a rough sea is made of.
    ///
    /// 0.30 is the default because a field is many trains and they sum: on the
    /// generator's own six-train sea, measured over a 80 m square across eight
    /// seconds, nothing folds at 0.35 and 14% of the surface folds at 1.0. So
    /// the default sits just under the measured ceiling for the sea the
    /// generator makes, and a sea of fewer or gentler trains can go far higher.
    /// The editor measures yours rather than guessing from this number.
    float steepness_headroom{0.30f};

    /// How these trains answer the weather's wind.
    ///
    /// Held here rather than once for the whole field because the answer is
    /// made of trains — which of them turn, how far the fold budget stretches
    /// — and because contributors are meant to differ in it: a swell raised
    /// by a storm somewhere else rolls through unmoved while the chop beside
    /// it answers the wind it is in.
    WaterWindResponse wind;
};

/// Simulated water: a wind-driven spectrum rather than a list of trains.
///
/// Where a Gerstner contributor is authored one wave at a time, this is a
/// statistical description — a wind, how far it has blown over open water,
/// and what the sea it raises is then shaped into. Groups, crest variety and
/// detail that does not repeat are what a real spectrum gives for nothing and
/// what no hand-written list of trains does.
///
/// The spectrum is JONSWAP: the measured shape of a sea still growing under
/// the wind that is raising it, which is every sea a game shows. The three
/// dials after the wind are stylisation on top of it, because a correct ocean
/// is busier than a readable one.
///
/// The defaults are a stylised sea rather than a physical one: the physical
/// amounts (height 1, choppiness 1, no high cut) are correct and read as tame,
/// which is not what a new contributor in this engine's art style wants to
/// open on. The spectrum under them is untouched — they are the dials.
struct FftContributor {
    /// Direction the wind blows across the ground plane, world XZ. Waves
    /// travel with it.
    glm::vec2 wind_direction{1.0f, 0.0f};

    /// Wind speed the spectrum is raised by, in m/s. 8 to 10 is a fresh breeze,
    /// about the reference a Gerstner contributor is authored at.
    float wind_speed{9.0f};

    /// Whether the weather's wind replaces the two above. Off, the sea is the
    /// one its own wind raises, whatever the weather does — a canal, a pool,
    /// a sea held still to be tuned. On, it is raised by the wind the weather
    /// is blowing, late: the same lag an authored sea answers through, so a
    /// front builds it over its passage rather than at its arrival.
    bool answers_weather{false};

    /// How far the wind has blown over open water to get here, in metres.
    /// With wind speed it decides how grown the sea is: a short fetch is a
    /// lake's steep short chop, a long one the open ocean's heavy swell under
    /// the same wind.
    float fetch{60000.0f};

    /// Half-angle, in degrees either side of the wind, at which the sea
    /// carries half the energy it carries straight downwind. Narrow it and
    /// the crests become long, parallel, graphic ridges; widen it and the sea
    /// turns confused.
    float spread_degrees{36.0f};

    /// Wavelength in metres below which the sea is faded out, or 0 to keep
    /// everything the grid can hold. The single biggest stylisation control:
    /// it is what turns a busy correct ocean into a few big readable swells.
    /// A fade rather than a wall, so nothing rings.
    float high_cut{1.6f};

    /// How far crests are pinched, as a multiplier on the horizontal
    /// displacement the spectrum implies. 0 is a heightfield of rolling
    /// hills, 1 the physical amount, and past it crests sharpen until they
    /// fold. The same motion a Gerstner train's steepness buys.
    float choppiness{3.0f};

    /// Multiplies the height of the whole simulated sea.
    float amplitude_scale{2.9f};

    /// The side of the square of sea that is simulated, in metres; it tiles
    /// beyond that. It wants to hold several of the longest waves the wind
    /// raises, or they are cut off, and few enough that the grid still
    /// resolves the short ones.
    float patch_size{256.0f};

    /// Samples along each side of the patch. A power of two.
    std::uint32_t resolution{256};

    /// Picks which sea of all the seas this spectrum describes. The same
    /// seed is the same water on every machine.
    std::uint32_t seed{1};

    /// How many of this sea's strongest waves physics feels. The simulation
    /// lives on the GPU, so a floating body is given the sea's strongest waves
    /// in closed form instead: the same waves, on the same clock, summed where
    /// the Gerstner trains are. 0 floats a body on the authored contributors
    /// alone. A few dozen carry most of a sea's height and none of its chop,
    /// which is about what a hull answers anyway.
    std::uint32_t physics_waves{0};

    [[nodiscard]] bool operator==(const FftContributor&) const = default;
};

/// One wavevector of a drawn sea as an ordinary travelling wave: the surface
/// it makes is `amplitude * cos(dot(k, xz) - omega * time + phase)`, moving the
/// way `k` points at the speed deep water gives a wave of that length.
///
/// The same wave the GPU transforms, written out so that it can be summed at a
/// single point without transforming anything: same amplitude, same phase, same
/// clock. That is what lets physics float a body on the simulated sea with no
/// renderer involved, and get the same answer on every machine.
struct SpectralWave {
    glm::vec2 k{0.0f};       // rad/m, world XZ
    float amplitude{0.0f};   // metres
    float phase{0.0f};       // radians
    float omega{0.0f};       // rad/s
    /// |k|. It follows from `k`; kept so a wave summed at thousands of points a
    /// tick does not take a square root at each. Never zero: the mean is not a
    /// wave. Only the length is kept and not, say, k/|k| or A/|k|, because the
    /// sums divide by it after the sine, and a product reassociated around a
    /// trig value rounds differently — the surface a body floats on has to be
    /// the same bits on every machine and the same bits as before.
    float k_len{1.0f};
};

namespace water {

/// The same waves as a `std::vector<SpectralWave>`, laid out for summing eight
/// at a time: each field of the wave in an array of its own, padded to a
/// multiple of eight with waves of zero amplitude, which add nothing.
///
/// A wave is summed at every sample point of every floating body every tick,
/// and the sum is the same arithmetic for every wave, which is what a CPU's
/// vector unit is for — but it wants the eight amplitudes it adds next to one
/// another, not a stride apart inside eight structs.
struct PackedWaves {
    std::vector<float> kx, kz, amplitude, phase, omega, k_len;
    std::size_t count{0};   // real waves; the arrays run to the next eight
};

/// One sea drawn from a spectrum: the complex amplitude of every wavevector
/// on the grid at time zero.
struct SpectrumField {
    /// Samples per side.
    std::uint32_t resolution{0};

    /// Spacing of the wavevector grid in rad/m: 2*pi / patch_size.
    float k_spacing{0.0f};

    /// Amplitudes, real in x and imaginary in y, row-major as `[z * resolution
    /// + x]`. In FFT order: index i along an axis is wavenumber i for the
    /// first half and i - resolution for the second, so the mean sits at 0.
    std::vector<glm::vec2> amplitudes;

    /// The wavevector index `i` along one axis stands for, in rad/m.
    [[nodiscard]] float wavenumber(std::uint32_t i) const;
};

} // namespace water

/// One source of surface motion in a `.fjwater`. The surface is the sum of
/// every contributor, so nothing downstream needs to know which kinds a sea
/// is made of.
using WaterContributor = std::variant<GerstnerContributor, FftContributor>;

/// A `.fjwater`: the wave field of a body of water.
///
/// The contributors are the whole runtime asset. Colour, foam look,
/// refraction and the rest of the surface's appearance live in its material,
/// because the split that matters is whether the CPU needs to know: buoyancy,
/// swimming and editor gizmos all ask where the surface is, and none of them
/// care what colour it is.
struct FjWaterData {
    /// What moves the surface, summed. Order changes nothing about the sea;
    /// it is the order an author listed them in. Gerstner contributors past
    /// MAX_GERSTNER_CONTRIBUTORS are not summed, and of the simulated ones
    /// only the first is: a simulation is a patch of sea carried forward every
    /// frame, and the world runs one.
    std::vector<WaterContributor> contributors;

    /// The `.fjmat` the water editor draws this field through, as an asset
    /// reference. Empty previews with the engine's own water shader.
    ///
    /// Authoring convenience: nothing at runtime reads it, and a placed body
    /// of water takes its material from its own component. It lives here
    /// because it belongs to the field being tuned — crest steepness and foam
    /// read differently under every material, so the one a sea was judged
    /// against is part of how to come back to it.
    std::string preview_material;

    /// The simulated contributor as it has been drawn: one sea out of the
    /// spectrum its settings describe, for the GPU to animate and, as far as it
    /// asks, for the CPU to float things on.
    ///
    /// Derived, never authored and never saved. Whoever derives the field the
    /// world runs on (`water::SeaDerivation`, or `water::apply_wind()` for a
    /// one-off) fills it, and a field straight from a file has none. It is one
    /// draw feeding both halves, which is what keeps a boat on the sea that is
    /// drawn: the renderer uploads `spectrum`, and physics sums `waves`, which
    /// are that same spectrum's strongest wavevectors.
    struct SimulatedSea {
        /// The contributor this was drawn from. It can trail the field's own
        /// by a few frames, while a new one is being shaped.
        FftContributor drawn_from;
        /// Shared and never changed once drawn, so handing the field around
        /// every frame copies a pointer and not a quarter of a million floats.
        std::shared_ptr<const water::SpectrumField> spectrum;
        /// `drawn_from.physics_waves` of its strongest waves, strongest first.
        std::vector<SpectralWave> waves;
        /// The same waves as the evaluators read them. Kept in step with
        /// `waves` by whoever sets it: `water::retune()`.
        water::PackedWaves packed;
        /// Scales the horizontal pinch, on both halves. The field's own and
        /// current, since it is applied as the sea is animated and costs no
        /// redraw to change.
        float choppiness{0.0f};
    };
    std::optional<SimulatedSea> simulated;
};

/// Fill a wave list from generator settings: wavelengths halving from the
/// longest, amplitudes falling with them, directions fanned across the wind,
/// and speeds drifting so the trains do not beat in lockstep.
///
/// Deterministic — the same settings always give the same list, so pressing
/// generate twice is not a reroll.
[[nodiscard]] std::vector<WaterWave> generate_water_waves(const WaterWaveGenerator& settings);

} // namespace fjell
