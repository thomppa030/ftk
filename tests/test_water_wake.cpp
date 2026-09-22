#include "environment/environment_state.hpp"
#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/water_gpu.hpp"
#include "renderer/resources/water_surface.hpp"
#include "renderer/resources/water_wake.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// The shader half of the wake, compiled as C++ so the two evaluators can be
// compared directly. See glsl_shim.hpp.
#include "glsl_shim.hpp"

// In a namespace of this file's own: test_fjwater.cpp compiles the wave
// field's shader too, and the two must not meet at link time.
namespace wake_shim {

struct FjellWaterField;
FjellWaterField& shim_water_field();
#define FJELL_WATER_FIELD shim_water_field()
#include "../shaders/include/fjell_water_waves.glsl"

struct FjellWakeSource;
FjellWakeSource* shim_wakes();
#define FJELL_WATER_WAKES shim_wakes()
#include "../shaders/include/fjell_water_wake.glsl"

FjellWaterField& shim_water_field() {
    static FjellWaterField field{};
    return field;
}

FjellWakeSource* shim_wakes() {
    static FjellWakeSource records[FJELL_MAX_WAKE_SOURCES]{};
    return records;
}

} // namespace wake_shim

using namespace wake_shim;

#include <bit>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

using namespace fjell;
using Catch::Approx;

namespace {

constexpr float GRAVITY = 9.81f;
constexpr float TAU = 6.28318530717958647692f;

/// A flat, derived field: what a body of water with no waves runs on.
[[nodiscard]] FjWaterData still_water() {
    FjWaterData water;
    water.contributors.emplace_back(GerstnerContributor{});
    return water::apply_wind(water, {0.0f, 0.0f});
}

[[nodiscard]] water::WakePlacement open_sea(float spacing = 1.0f) {
    water::WakePlacement placement;
    placement.surface_y = 0.0f;
    placement.vertex_spacing = spacing;
    return placement;
}

[[nodiscard]] MotionSource mover(std::uint32_t node, glm::vec3 position, glm::vec3 velocity,
                                 float radius, float strength = 1.0f) {
    MotionSource m;
    m.node_id = node;
    m.position = position;
    m.velocity = velocity;
    m.radius = radius;
    m.height = 4.0f * radius;
    m.bottom = 0.0f;
    m.strength = strength;
    return m;
}

/// Walk `m` at its velocity for `seconds`, one advance per `dt`, starting
/// from its position at `t0`. Returns the trails and the time reached.
struct Walk {
    water::WakeTrails trails;
    float time{0.0f};
};

void walk(Walk& w, MotionSource m, float seconds, float dt, const FjWaterData& sea,
          const water::WakePlacement& placement) {
    const int steps = static_cast<int>(std::round(seconds / dt));
    for (int i = 0; i < steps; ++i) {
        w.time += dt;
        m.position += m.velocity * dt;
        w.trails.advance(std::span<const MotionSource>(&m, 1), sea, placement, w.time);
    }
}

/// Put `wakes` where the shader reads them.
void upload(std::span<const water::WakeSource> wakes) {
    static_assert(sizeof(FjellWakeSource) == sizeof(GpuWakeSource));
    for (std::size_t i = 0; i < wakes.size(); ++i) {
        const GpuWakeSource packed = to_gpu_wake(wakes[i]);
        std::memcpy(&shim_wakes()[i], &packed, sizeof(packed));
    }
}

/// The wavelength, ring speed and birth amplitude a source of this radius and
/// speed gets, from the look's constants — worked out here rather than read
/// back from the record, so the record is being checked against arithmetic.
struct Expected {
    float wavelength;
    float ring_speed;
    float amplitude;
    float half_width;
};

[[nodiscard]] Expected expected(float radius, float speed, float strength = 1.0f) {
    Expected e;
    e.wavelength = std::max(4.0f * radius, water::WakeLook::MIN_WAVELENGTH);
    e.ring_speed = std::sqrt(GRAVITY * e.wavelength / TAU);
    e.amplitude = strength * water::WakeLook::AMPLITUDE_PER_RADIUS * radius
                * std::clamp(speed / e.ring_speed, 0.0f, water::WakeLook::MAX_SPEED_SHARE);
    e.half_width = e.wavelength * water::WakeLook::CREST_SHARE;
    return e;
}

} // namespace

// --- The two evaluators agree ---

TEST_CASE("Shader and CPU wakes agree", "[water_wake]") {
    const FjWaterData sea = still_water();
    Walk w;
    // A boat crossing at an angle, then a second, slower and smaller one
    // behind it, both wide enough to move the mesh.
    walk(w, mover(1, {-12.0f, 0.0f, -6.0f}, {5.0f, 0.0f, 2.0f}, 2.5f), 3.0f, 1.0f / 60.0f, sea, open_sea());
    MotionSource second = mover(2, {4.0f, 0.0f, 8.0f}, {-2.0f, 0.0f, -1.5f}, 1.2f);
    for (int i = 0; i < 90; ++i) {
        w.time += 1.0f / 60.0f;
        second.position += second.velocity / 60.0f;
        const MotionSource both[2] = {mover(1, {3.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 2.0f}, 2.5f), second};
        w.trails.advance(both, sea, open_sea(), w.time);
    }
    const auto& wakes = w.trails.sources();
    REQUIRE(wakes.size() == 2);
    REQUIRE(wakes[0].displaces);
    REQUIRE(wakes[1].displaces);
    upload(wakes);

    int compared = 0;
    for (float time : {w.time, w.time + 0.3f, w.time + 1.7f}) {
        for (float x = -30.0f; x <= 30.0f; x += 3.0f) {
            for (float z = -30.0f; z <= 30.0f; z += 3.0f) {
                const glm::vec2 p(x, z);
                const water::WakeSurface cpu = water::wake_surface(wakes, p, time);
                const FjellWakeSurface gpu = fjell_water_wake_surface(0u, 2u, p, time, 0.0f);
                REQUIRE(gpu.height == Approx(cpu.height).margin(1e-4));
                REQUIRE(gpu.slope.x == Approx(cpu.slope.x).margin(1e-4));
                REQUIRE(gpu.slope.y == Approx(cpu.slope.y).margin(1e-4));
                if (std::abs(cpu.height) > 1e-3f) ++compared;
            }
        }
    }
    // The grid actually crossed the wake, not only the flat water around it.
    REQUIRE(compared > 50);
}

TEST_CASE("The whole field agrees with the wake in it", "[water_wake]") {
    // A sea with a train, and a wake on it: what the mesh shader sums against
    // what buoyancy sums.
    FjWaterData authored;
    GerstnerContributor g;
    WaterWave train;
    train.direction = {0.6f, 0.8f};
    train.wavelength = 14.0f;
    train.amplitude = 0.4f;
    train.steepness = 0.5f;
    g.waves.push_back(train);
    authored.contributors.emplace_back(g);
    FjWaterData sea = water::apply_wind(authored, {0.0f, 0.0f});

    Walk w;
    walk(w, mover(3, {-10.0f, 0.0f, 0.0f}, {6.0f, 0.0f, 0.0f}, 2.0f), 3.0f, 1.0f / 60.0f, sea, open_sea());
    sea.wakes = w.trails.sources();
    upload(sea.wakes);
    shim_water_field() = std::bit_cast<FjellWaterField>(to_gpu_field(sea));

    for (float x = -8.0f; x <= 12.0f; x += 2.0f) {
        for (float z = -8.0f; z <= 8.0f; z += 2.0f) {
            const glm::vec2 p(x, z);
            const float time = w.time;
            const glm::vec3 cpu = water::displace(sea, p, time);
            const vec3 trains = fjell_water_displace(p, time);
            const FjellWakeSurface wake = fjell_water_wake_surface(0u, 1u, p, time, 0.0f);
            REQUIRE(trains.x == Approx(cpu.x).margin(1e-4));
            REQUIRE(trains.y + wake.height == Approx(cpu.y).margin(1e-4));
            REQUIRE(trains.z == Approx(cpu.z).margin(1e-4));

            FjellWaterTangents t = fjell_water_tangents(p, time);
            t.binormal.y += wake.slope.x;
            t.tangent.y += wake.slope.y;
            const vec3 gpu_n = fjell_water_normal_from(t);
            const glm::vec3 cpu_n = water::normal_at(sea, p, time);
            REQUIRE(gpu_n.x == Approx(cpu_n.x).margin(1e-4));
            REQUIRE(gpu_n.y == Approx(cpu_n.y).margin(1e-4));
            REQUIRE(gpu_n.z == Approx(cpu_n.z).margin(1e-4));
        }
    }
}

TEST_CASE("The GPU wake record matches the shader's struct", "[water_wake]") {
    static_assert(sizeof(GpuWakeSource) == sizeof(FjellWakeSource));
    static_assert(water::MAX_WAKE_STAMPS == FJELL_MAX_WAKE_STAMPS);
    static_assert(water::MAX_WAKE_SOURCES == FJELL_MAX_WAKE_SOURCES);

    water::WakeSource s;
    s.position = {1.0f, 2.0f};
    s.velocity = {3.0f, 4.0f};
    s.bounds_min = {-5.0f, -6.0f};
    s.bounds_max = {7.0f, 8.0f};
    s.k = 0.5f;
    s.ring_speed = 3.5f;
    s.width = 2.5f;
    s.decay = 2.0f;
    s.radius = 1.5f;
    s.time = 11.0f;
    s.displaces = true;
    s.stamps.push_back({{9.0f, 10.0f}, 10.5f, 0.07f});
    s.stamps.push_back({{11.0f, 12.0f}, 10.9f, 0.08f});

    FjellWakeSource shader{};
    const GpuWakeSource packed = to_gpu_wake(s);
    std::memcpy(&shader, &packed, sizeof(packed));
    REQUIRE(shader.position == vec4(1.0f, 2.0f, 3.0f, 4.0f));
    REQUIRE(shader.bounds == vec4(-5.0f, -6.0f, 7.0f, 8.0f));
    REQUIRE(shader.ring == vec4(0.5f, 3.5f, 2.5f, 2.0f));
    REQUIRE(shader.body == vec4(1.5f, 11.0f, 0.0f, 0.0f));
    REQUIRE(shader.counts.x == 2u);
    REQUIRE(shader.counts.y == 1u);
    REQUIRE(shader.stamps[1] == vec4(11.0f, 12.0f, 10.9f, 0.08f));
}

// --- Checked against arithmetic, not only against the twin ---

TEST_CASE("The wake's slope is the derivative of its height", "[water_wake]") {
    const FjWaterData sea = still_water();
    Walk w;
    walk(w, mover(1, {-8.0f, 0.0f, -3.0f}, {4.0f, 0.0f, 1.5f}, 2.0f), 3.0f, 1.0f / 60.0f, sea, open_sea());
    upload(w.trails.sources());

    const float e = 2e-3f;
    int steep = 0;
    for (float x = -20.0f; x <= 20.0f; x += 2.5f) {
        for (float z = -20.0f; z <= 20.0f; z += 2.5f) {
            const vec2 p(x, z);
            const FjellWakeSurface at = fjell_water_wake_surface(0u, 1u, p, w.time, 0.0f);
            const float dx = (fjell_water_wake_surface(0u, 1u, p + vec2(e, 0.0f), w.time, 0.0f).height
                            - fjell_water_wake_surface(0u, 1u, p - vec2(e, 0.0f), w.time, 0.0f).height) / (2.0f * e);
            const float dz = (fjell_water_wake_surface(0u, 1u, p + vec2(0.0f, e), w.time, 0.0f).height
                            - fjell_water_wake_surface(0u, 1u, p - vec2(0.0f, e), w.time, 0.0f).height) / (2.0f * e);
            REQUIRE(at.slope.x == Approx(dx).margin(5e-3));
            REQUIRE(at.slope.y == Approx(dz).margin(5e-3));
            if (std::abs(dx) + std::abs(dz) > 0.05f) ++steep;
        }
    }
    REQUIRE(steep > 5);
}

TEST_CASE("The wake's velocity is the derivative of its height in time", "[water_wake]") {
    const FjWaterData sea = still_water();
    Walk w;
    walk(w, mover(1, {-8.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}, 2.5f), 3.0f, 1.0f / 60.0f, sea, open_sea());
    const auto& wakes = w.trails.sources();

    const float e = 1e-3f;
    int moving = 0;
    for (float x = -20.0f; x <= 20.0f; x += 2.5f) {
        for (float z = -12.0f; z <= 12.0f; z += 2.5f) {
            const glm::vec2 p(x, z);
            const water::WakeSurface at = water::wake_surface(wakes, p, w.time + 0.5f);
            const float dt = (water::wake_surface(wakes, p, w.time + 0.5f + e).height
                            - water::wake_surface(wakes, p, w.time + 0.5f - e).height) / (2.0f * e);
            REQUIRE(at.velocity == Approx(dt).margin(1e-2));
            if (std::abs(dt) > 0.05f) ++moving;
        }
    }
    REQUIRE(moving > 20);
}

TEST_CASE("A ring's crest is where its speed and age put it", "[water_wake]") {
    const FjWaterData sea = still_water();
    const float radius = 1.0f;
    const float speed = 2.0f;
    Walk w;
    // One frame moving lays one stamp; then the source is gone, and only the
    // ring is left.
    MotionSource m = mover(5, {0.0f, 0.0f, 0.0f}, {speed, 0.0f, 0.0f}, radius);
    w.time = 1.0f;
    w.trails.advance(std::span<const MotionSource>(&m, 1), sea, open_sea(), w.time);
    REQUIRE(w.trails.sources().size() == 1);
    REQUIRE(w.trails.sources()[0].stamps.size() == 1);
    w.trails.advance({}, sea, open_sea(), w.time + 0.01f);
    REQUIRE(w.trails.sources().size() == 1);

    const Expected want = expected(radius, speed);
    const float age = 2.0f;
    const float time = 1.0f + age;
    // The crest sits where sqrt(r² + ρ²) == c·age.
    const float crest_r = std::sqrt(want.ring_speed * age * want.ring_speed * age - radius * radius);
    float best_r = 0.0f;
    float best_h = -1.0f;
    for (float r = 0.0f; r < 20.0f; r += 0.01f) {
        const float h = water::wake_surface(w.trails.sources()[0], {r, 0.0f}, time).height;
        if (h > best_h) {
            best_h = h;
            best_r = r;
        }
    }
    REQUIRE(best_r == Approx(crest_r).margin(0.05));
    // And stands at the birth amplitude, decayed and spread over the circumference.
    const float want_h = want.amplitude * std::exp(-age / water::WakeLook::DECAY_SECONDS)
                       / std::sqrt(1.0f + want.ring_speed * age / radius);
    REQUIRE(best_h == Approx(want_h).epsilon(0.02));
}

TEST_CASE("Rings run ahead of a slow source and never ahead of a fast one", "[water_wake]") {
    const FjWaterData sea = still_water();
    const float radius = 1.0f;
    const Expected slow = expected(radius, 1.0f);
    REQUIRE(1.0f < slow.ring_speed);   // the wader is slower than its rings
    REQUIRE(6.0f > slow.ring_speed);   // the boat is faster

    auto ahead = [&](float speed, float seconds) {
        Walk w;
        walk(w, mover(1, {0.0f, 0.0f, 0.0f}, {speed, 0.0f, 0.0f}, radius), seconds, 1.0f / 60.0f, sea, open_sea());
        const water::WakeSource& s = w.trails.sources().at(0);
        float most = 0.0f;
        // Straight ahead, well past the body.
        for (float d = 8.0f; d <= 12.0f; d += 0.1f) {
            const glm::vec2 p = s.position + glm::vec2(d, 0.0f);
            most = std::max(most, std::abs(water::wake_surface(s, p, w.time).height));
        }
        return most;
    };
    // A ring laid five seconds ago by the wader is about a millimetre high
    // out here once decayed and spread; ask for a tenth of that. Ahead of
    // the boat there is nothing at all.
    REQUIRE(ahead(1.0f, 6.0f) > 1e-4f);
    REQUIRE(ahead(6.0f, 3.0f) == 0.0f);
}

TEST_CASE("A coarse grid fades a wake it cannot draw", "[water_wake]") {
    const FjWaterData sea = still_water();
    Walk w;
    walk(w, mover(1, {-6.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}, 2.5f), 2.0f, 1.0f / 60.0f, sea, open_sea());
    upload(w.trails.sources());
    const float half_width = expected(2.5f, 5.0f).half_width;

    // A point with wake under it: search the trail's neighbourhood for one.
    vec2 p(0.0f);
    float full = 0.0f;
    for (float x = -20.0f; x <= 20.0f && std::abs(full) < 1e-2f; x += 0.5f) {
        for (float z = -10.0f; z <= 10.0f && std::abs(full) < 1e-2f; z += 0.5f) {
            p = vec2(x, z);
            full = fjell_water_wake_surface(0u, 1u, p, w.time, 0.0f).height;
        }
    }
    REQUIRE(std::abs(full) >= 1e-2f);
    REQUIRE(fjell_water_wake_surface(0u, 1u, p, w.time, half_width * 0.5f).height == Approx(full));
    REQUIRE(fjell_water_wake_surface(0u, 1u, p, w.time, half_width * 0.75f).height == Approx(full * 0.5f).margin(1e-6));
    REQUIRE(fjell_water_wake_surface(0u, 1u, p, w.time, half_width).height == 0.0f);
}

// --- Laying a trail ---

TEST_CASE("A trail is laid by distance, not by frame", "[water_wake]") {
    const FjWaterData sea = still_water();
    const float radius = 0.4f;
    const float speed = 1.5f;
    const Expected e = expected(radius, speed);
    const float spacing = e.wavelength * water::WakeLook::STAMP_SPACING_WAVELENGTHS;

    // 6.15 m: not a whole number of spacings, so rounding cannot decide
    // whether the last stamp is laid.
    Walk fast;
    walk(fast, mover(1, {0.0f, -0.5f, 0.0f}, {0.0f, 0.0f, speed}, radius), 4.1f, 1.0f / 60.0f, sea, open_sea());
    Walk slow;
    walk(slow, mover(1, {0.0f, -0.5f, 0.0f}, {0.0f, 0.0f, speed}, radius), 4.1f, 1.0f / 10.0f, sea, open_sea());

    // The first stamp is laid where the source is first seen moving, which
    // is a frame in; from there on it is one every spacing along the way,
    // however the frames fall.
    for (const water::WakeTrails* trails : {&fast.trails, &slow.trails}) {
        const auto& stamps = trails->sources().at(0).stamps;
        const float end = 4.1f * speed;
        const std::size_t want =
            1 + static_cast<std::size_t>(std::floor((end - stamps[0].position.y) / spacing + 1e-4f));
        REQUIRE(stamps.size() == want);
        for (std::size_t i = 1; i < want; ++i) {
            REQUIRE(stamps[i].position.y - stamps[i - 1].position.y == Approx(spacing).margin(1e-4));
            REQUIRE(stamps[i].position.x == 0.0f);
            REQUIRE(stamps[i].amplitude == Approx(e.amplitude));
        }
    }
}

TEST_CASE("A body standing in the water lays nothing", "[water_wake]") {
    const FjWaterData sea = still_water();
    Walk w;
    walk(w, mover(1, {0.0f, -0.5f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.4f), 2.0f, 1.0f / 60.0f, sea, open_sea());
    REQUIRE(w.trails.sources().size() == 1);
    REQUIRE(w.trails.sources()[0].stamps.empty());
    REQUIRE(water::wake_surface(w.trails.sources()[0], {0.5f, 0.5f}, w.time).height == 0.0f);
}

TEST_CASE("Stamps die of age and a gone source is forgotten with them", "[water_wake]") {
    const FjWaterData sea = still_water();
    Walk w;
    walk(w, mover(1, {0.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}, 2.5f), 3.0f, 1.0f / 60.0f, sea, open_sea());
    const std::size_t laid = w.trails.sources().at(0).stamps.size();
    REQUIRE(laid > 1);

    w.trails.advance({}, sea, open_sea(), w.time + 0.01f);
    REQUIRE(w.trails.sources().size() == 1);
    REQUIRE(w.trails.sources()[0].stamps.size() == laid);

    // Half a lifetime past the first stamp, some are gone and some remain.
    w.trails.advance({}, sea, open_sea(), 1.0f / 60.0f + water::WakeLook::LIFETIME_SECONDS + 0.4f);
    REQUIRE(w.trails.sources().size() == 1);
    REQUIRE(w.trails.sources()[0].stamps.size() < laid);
    REQUIRE(!w.trails.sources()[0].stamps.empty());

    w.trails.advance({}, sea, open_sea(), w.time + water::WakeLook::LIFETIME_SECONDS + 0.01f);
    REQUIRE(w.trails.sources().empty());
}

TEST_CASE("A source above or beside the water leaves it alone", "[water_wake]") {
    const FjWaterData sea = still_water();
    Walk w;
    // Walking along a cliff three metres up.
    walk(w, mover(1, {0.0f, 3.0f, 0.0f}, {1.5f, 0.0f, 0.0f}, 0.4f), 1.0f, 1.0f / 60.0f, sea, open_sea());
    REQUIRE(w.trails.sources().empty());
    // Wading, feet half a metre under.
    walk(w, mover(1, {0.0f, -0.5f, 0.0f}, {1.5f, 0.0f, 0.0f}, 0.4f), 1.0f, 1.0f / 60.0f, sea, open_sea());
    REQUIRE(w.trails.sources().size() == 1);
    // A pond a hundred metres across, and a walker beyond its edge.
    water::WakePlacement pond = open_sea();
    pond.square = std::make_pair(glm::vec2(-50.0f), glm::vec2(50.0f));
    Walk p;
    walk(p, mover(2, {60.0f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.5f}, 0.4f), 1.0f, 1.0f / 60.0f, sea, pond);
    REQUIRE(p.trails.sources().empty());
    walk(p, mover(2, {10.0f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.5f}, 0.4f), 1.0f, 1.0f / 60.0f, sea, pond);
    REQUIRE(p.trails.sources().size() == 1);
}

TEST_CASE("A jump starts the trail again", "[water_wake]") {
    const FjWaterData sea = still_water();
    Walk w;
    walk(w, mover(1, {0.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}, 2.5f), 4.0f, 1.0f / 60.0f, sea, open_sea());
    REQUIRE(w.trails.sources().at(0).stamps.size() > 3);
    // Teleported a kilometre on.
    MotionSource m = mover(1, {1000.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}, 2.5f);
    w.trails.advance(std::span<const MotionSource>(&m, 1), sea, open_sea(), w.time + 1.0f / 60.0f);
    REQUIRE(w.trails.sources().at(0).stamps.size() == 1);
    REQUIRE(w.trails.sources()[0].stamps[0].position.x == Approx(1000.0f));
}

TEST_CASE("The water follows at most MAX_WAKE_SOURCES movers", "[water_wake]") {
    const FjWaterData sea = still_water();
    std::vector<MotionSource> crowd;
    for (std::uint32_t i = 0; i < water::MAX_WAKE_SOURCES + 5; ++i) {
        crowd.push_back(mover(i + 1, {static_cast<float>(i) * 10.0f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.5f}, 0.4f));
    }
    water::WakeTrails trails;
    trails.advance(crowd, sea, open_sea(), 1.0f);
    REQUIRE(trails.sources().size() == water::MAX_WAKE_SOURCES);
    REQUIRE(trails.sources().back().node_id == water::MAX_WAKE_SOURCES);
}

// --- Physics ---

TEST_CASE("A body does not feel its own wake", "[water_wake]") {
    FjWaterData sea = still_water();
    Walk w;
    walk(w, mover(7, {-8.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}, 2.5f), 3.0f, 1.0f / 60.0f, sea, open_sea());
    sea.wakes = w.trails.sources();

    const glm::vec2 astern = sea.wakes[0].position - glm::vec2(6.0f, 1.0f);
    const water::SurfaceMotion own = water::surface_motion(sea, astern, w.time, 7);
    const water::SurfaceMotion other = water::surface_motion(sea, astern, w.time, 8);
    const water::SurfaceMotion anyone = water::surface_motion(sea, astern, w.time);
    REQUIRE(own.displacement.y == 0.0f);
    REQUIRE(own.velocity.y == 0.0f);
    REQUIRE(std::abs(other.displacement.y) > 1e-3f);
    REQUIRE(other.displacement.y == anyone.displacement.y);
}

TEST_CASE("Nothing floats on a swimmer's ripple", "[water_wake]") {
    FjWaterData sea = still_water();
    Walk w;
    // Narrower than the metre between vertices: shades the water, never moves it.
    walk(w, mover(1, {-3.0f, -0.5f, 0.0f}, {2.0f, 0.0f, 0.0f}, 0.4f), 3.0f, 1.0f / 60.0f, sea, open_sea(1.0f));
    const water::WakeSource& swimmer = w.trails.sources().at(0);
    REQUIRE(!swimmer.displaces);
    const glm::vec2 behind = swimmer.position - glm::vec2(1.0f, 0.2f);
    REQUIRE(std::abs(water::wake_surface(swimmer, behind, w.time).height) > 1e-4f);
    REQUIRE(water::wake_surface(w.trails.sources(), behind, w.time).height == 0.0f);

    sea.wakes = w.trails.sources();
    REQUIRE(water::surface_motion(sea, behind, w.time).displacement.y == 0.0f);

    upload(sea.wakes);
    REQUIRE(fjell_water_wake_surface(0u, 1u, behind, w.time, 0.0f).height == 0.0f);
    float foam = 0.0f;
    float tilt = 0.0f;
    for (float back = 0.2f; back < 4.0f; back += 0.05f) {
        const FjellWakeShade shade = fjell_water_wake_shade(
            0u, 1u, swimmer.position - glm::vec2(back, 0.2f), w.time, 0.0f);
        foam = std::max(foam, shade.foam);
        tilt = std::max(tilt, std::abs(shade.slope.x) + std::abs(shade.slope.y));
    }
    REQUIRE(tilt > 1e-3f);
    REQUIRE(foam > 0.3f);

    // The same swimmer on a pond drawn finely enough moves it.
    Walk fine;
    walk(fine, mover(1, {-3.0f, -0.5f, 0.0f}, {2.0f, 0.0f, 0.0f}, 0.4f), 3.0f, 1.0f / 60.0f, sea, open_sea(0.25f));
    REQUIRE(fine.trails.sources().at(0).displaces);
}

TEST_CASE("A wake out of reach leaves the surface bit for bit", "[water_wake]") {
    FjWaterData authored;
    GerstnerContributor g;
    WaterWave train;
    train.wavelength = 9.0f;
    train.amplitude = 0.3f;
    g.waves.push_back(train);
    authored.contributors.emplace_back(g);
    FjWaterData sea = water::apply_wind(authored, {0.0f, 0.0f});
    const glm::vec2 p(3.3f, -7.1f);
    const water::SurfaceMotion before = water::surface_motion(sea, p, 4.2f);
    const glm::vec3 normal_before = water::normal_at(sea, p, 4.2f);

    Walk w;
    walk(w, mover(1, {500.0f, 0.0f, 500.0f}, {5.0f, 0.0f, 0.0f}, 2.5f), 2.0f, 1.0f / 60.0f, sea, open_sea());
    sea.wakes = w.trails.sources();
    const water::SurfaceMotion after = water::surface_motion(sea, p, 4.2f);
    const glm::vec3 normal_after = water::normal_at(sea, p, 4.2f);
    REQUIRE(std::memcmp(&before, &after, sizeof(before)) == 0);
    REQUIRE(std::memcmp(&normal_before, &normal_after, sizeof(normal_before)) == 0);
}

// --- Churn ---

TEST_CASE("Foam rides the crests and dies with them", "[water_wake]") {
    const FjWaterData sea = still_water();
    Walk w;
    walk(w, mover(1, {-10.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}, 2.5f), 2.0f, 1.0f / 60.0f, sea, open_sea());
    upload(w.trails.sources());
    const water::WakeSource& s = w.trails.sources()[0];

    // Along a ray from the oldest stamp, foam is where the crest is, and
    // nowhere between crests.
    const water::WakeStamp& oldest = s.stamps.front();
    const float age = w.time - oldest.time;
    const float crest_r = std::sqrt(s.ring_speed * age * s.ring_speed * age - s.radius * s.radius);
    // Straight off to the side, where no other stamp's ring is.
    const vec2 on_crest = vec2(oldest.position) + vec2(0.0f, crest_r);
    const vec2 between = vec2(oldest.position) + vec2(0.0f, crest_r + 2.0f * s.width);
    const float at_crest = fjell_water_wake_shade(0u, 1u, on_crest, w.time, 0.0f).foam;
    const float off_crest = fjell_water_wake_shade(0u, 1u, between, w.time, 0.0f).foam;
    // What the crest has left of its birth height, then the foam's own fade.
    const float want = std::exp(-age / water::WakeLook::DECAY_SECONDS)
                     / std::sqrt(1.0f + s.ring_speed * age / s.radius)
                     * std::exp(-age / water::WakeLook::FOAM_DECAY_SECONDS);
    REQUIRE(at_crest == Approx(want).epsilon(0.05));
    REQUIRE(off_crest == 0.0f);

    // A fresh crest foams more than an old one, and the foam is gone with
    // the stamps.
    const water::WakeStamp& newest = s.stamps.back();
    const float fresh_age = w.time - newest.time;
    const float fresh_r = std::sqrt(std::max(
        s.ring_speed * fresh_age * s.ring_speed * fresh_age - s.radius * s.radius, 0.0f));
    const vec2 fresh = vec2(newest.position) + vec2(0.0f, fresh_r);
    REQUIRE(fjell_water_wake_shade(0u, 1u, fresh, w.time, 0.0f).foam > at_crest);
    REQUIRE(fjell_water_wake_shade(0u, 1u, on_crest, w.time + 20.0f, 0.0f).foam < 1e-3f);
}

TEST_CASE("A ripple narrower than the pixels fades out", "[water_wake]") {
    const FjWaterData sea = still_water();
    Walk w;
    walk(w, mover(1, {-3.0f, -0.5f, 0.0f}, {2.0f, 0.0f, 0.0f}, 0.4f), 3.0f, 1.0f / 60.0f, sea, open_sea());
    upload(w.trails.sources());
    const float half_width = expected(0.4f, 2.0f).half_width;
    // A point on a crest: search behind the swimmer for one.
    vec2 behind(0.0f);
    FjellWakeShade sharp;
    sharp.slope = vec2(0.0f);
    sharp.foam = 0.0f;
    for (float back = 0.2f; back < 6.0f && std::abs(sharp.slope.x) < 1e-3f; back += 0.05f) {
        behind = w.trails.sources()[0].position - vec2(back, 0.2f);
        sharp = fjell_water_wake_shade(0u, 1u, behind, w.time, 0.0f);
    }
    const FjellWakeShade coarse = fjell_water_wake_shade(0u, 1u, behind, w.time, half_width * 0.5f);
    REQUIRE(std::abs(sharp.slope.x) >= 1e-3f);
    REQUIRE(coarse.slope.x == 0.0f);
    REQUIRE(coarse.slope.y == 0.0f);
    REQUIRE(coarse.foam == sharp.foam);
}
