#include "physics/buoyancy.hpp"
#include "physics/physics_types.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace fjell;
using Catch::Approx;

namespace {

constexpr float GRAVITY = 9.81f;

[[nodiscard]] ColliderComponent box(glm::vec3 half) {
    ColliderComponent c;
    c.shape = ColliderShape::Box;
    c.half_extents = half;
    return c;
}

/// Deep enough that the waterline ramp is fully on, so a test about the
/// force is not really a test about the ramp.
constexpr float DEEP = 1.0f;

/// One point's force with nothing moving, over a one-metre span.
[[nodiscard]] BuoyancyForce at_rest(float depth, float volume, float drag = 0.0f) {
    return buoyancy_at(depth, volume, 1.0f, drag, glm::vec3(0.0f), glm::vec3(0.0f),
                       GRAVITY);
}

} // namespace

TEST_CASE("A point clear of the water does nothing", "[buoyancy]") {
    // A body out of the water is a body in the air; only submersion lifts.
    // Clear means clear of the slab the point stands for, so half a span
    // above the surface rather than a hair above it.
    REQUIRE_FALSE(at_rest(-2.0f, 1.0f).submerged);
    REQUIRE(at_rest(-2.0f, 1.0f).force == glm::vec3(0.0f));

    // And exactly at the surface the slab is half under, which is the whole
    // point of centring it: a body settles smoothly instead of switching.
    REQUIRE(at_rest(0.0f, 1.0f).force.y == Approx(0.5f * WATER_DENSITY * GRAVITY));
}

TEST_CASE("Lift is the weight of the water displaced", "[buoyancy]") {
    // And nothing else. The body's own weight is gravity's to apply and the
    // solver already does; taking it off here too would have every float
    // carry its weight twice and sink whatever it was made of. That bug read
    // as "buoyancy does not work" and cost a rewrite of this function.
    const BuoyancyForce f = at_rest(DEEP, 1.0f);
    REQUIRE(f.submerged);
    REQUIRE(f.force.y == Approx(1.0f * WATER_DENSITY * GRAVITY));

    // Twice the displacement, twice the lift. Nothing about the body enters.
    REQUIRE(at_rest(DEEP, 2.0f).force.y == Approx(2.0f * WATER_DENSITY * GRAVITY));
}

TEST_CASE("Lift stops at the water a body displaces", "[buoyancy]") {
    // Archimedes, not a spring. Once a point is fully under, pushing it
    // deeper does not push back harder — otherwise a body held down would be
    // fired out of the water when released, and a stone would find a depth
    // where the water held it up.
    const float just_under = at_rest(1.0f, 1.0f).force.y;
    const float far_under = at_rest(50.0f, 1.0f).force.y;
    REQUIRE(far_under == Approx(just_under));
}

TEST_CASE("A point fades in across its own span", "[buoyancy]") {
    // A point stands for a slice of the body, and a slice goes under
    // gradually. Switching a whole share on in one frame makes a body resting
    // at the waterline buzz as the waves flick its points in and out.
    const float full = at_rest(1.0f, 1.0f).force.y;
    // Centred on the point: none half a span above, half at the surface, all
    // half a span below.
    REQUIRE(at_rest(-0.5f, 1.0f).force.y == Approx(0.0f));
    REQUIRE(at_rest(0.0f, 1.0f).force.y == Approx(full * 0.5f));
    REQUIRE(at_rest(0.5f, 1.0f).force.y == Approx(full));
}

TEST_CASE("The span is the body, not a fixed distance", "[buoyancy]") {
    // A point on a barge stands for a much thicker slice than one on a crate.
    // With a constant span a big hull saturates the moment it touches the
    // water and settles far too high.
    const float crate = buoyancy_at(0.5f, 1.0f, 1.0f, 0.0f, glm::vec3(0.0f),
                                    glm::vec3(0.0f), GRAVITY).force.y;
    const float barge = buoyancy_at(0.5f, 1.0f, 4.0f, 0.0f, glm::vec3(0.0f),
                                    glm::vec3(0.0f), GRAVITY).force.y;
    // Half a metre under puts the crate's slab fully under and the barge's
    // only a little past its middle.
    REQUIRE(crate == Approx(1.0f * WATER_DENSITY * GRAVITY));
    REQUIRE(barge == Approx(0.625f * WATER_DENSITY * GRAVITY));
}

TEST_CASE("Water resists a body moving through it", "[buoyancy]") {
    // Without drag a float dropped in water oscillates forever, because
    // nothing takes the energy back out.
    const glm::vec3 sinking{0.0f, -3.0f, 0.0f};
    const BuoyancyForce moving = buoyancy_at(DEEP, 1.0f, 1.0f, 2.0f, sinking,
                                             glm::vec3(0.0f), GRAVITY);
    const BuoyancyForce still = at_rest(DEEP, 1.0f);
    // Sinking, so the drag pushes back up: more than the lift alone.
    REQUIRE(moving.force.y > still.force.y);
}

TEST_CASE("A body is dragged against the water, not the world", "[buoyancy]") {
    // A raft sitting still in a moving current should be carried along by it.
    // Measured against the world it would instead be held in place and the
    // current would do nothing.
    const glm::vec3 current{2.0f, 0.0f, 0.0f};
    const BuoyancyForce f = buoyancy_at(DEEP, 1.0f, 1.0f, 2.0f, glm::vec3(0.0f), current,
                                        GRAVITY);
    REQUIRE(f.force.x > 0.0f);

    // And a body already moving with the current feels nothing from it.
    const BuoyancyForce carried = buoyancy_at(DEEP, 1.0f, 1.0f, 2.0f, current, current,
                                              GRAVITY);
    REQUIRE(carried.force.x == Approx(0.0f));
}

TEST_CASE("A box floats on its corners", "[buoyancy]") {
    // Eight points, so a crate tips over an edge rather than pivoting about
    // its middle. One point could only ever heave.
    BuoyancyComponent b;
    const ColliderComponent c = box({0.5f, 0.5f, 0.5f});
    const BuoyancySamples s = buoyancy_samples(b, &c);

    REQUIRE(s.points.size() == 8);
    REQUIRE(s.volume == Approx(1.0f));
    for (const auto& p : s.points) {
        // Full width, so it tips over an edge.
        REQUIRE(std::abs(p.x) == Approx(0.5f));
        REQUIRE(std::abs(p.z) == Approx(0.5f));
        // Half height: each layer stands for a slab and a slab is weighed at
        // its middle, not at the body's skin.
        REQUIRE(std::abs(p.y) == Approx(0.25f));
    }
    // And that slab is half the body.
    REQUIRE(s.span == Approx(0.5f));
}

TEST_CASE("Sampling finer does not float a body higher", "[buoyancy]") {
    // The volume is shared between the points, so resolution and lift are
    // separate. If they were not, authoring more points would make a hull
    // ride higher and every float would need retuning.
    BuoyancyComponent coarse;
    coarse.points = {{0.0f, 0.0f, 0.0f}};
    BuoyancyComponent fine;
    fine.points = {{-0.4f, 0.0f, 0.0f}, {0.4f, 0.0f, 0.0f},
                   {0.0f, 0.0f, -0.4f}, {0.0f, 0.0f, 0.4f}};

    const ColliderComponent c = box({0.5f, 0.5f, 0.5f});
    const BuoyancySamples a = buoyancy_samples(coarse, &c);
    const BuoyancySamples b = buoyancy_samples(fine, &c);
    REQUIRE(a.volume == Approx(b.volume));

    auto total_lift = [](const BuoyancySamples& s) {
        float sum = 0.0f;
        const float share = s.volume / static_cast<float>(s.points.size());
        for (std::size_t i = 0; i < s.points.size(); ++i) {
            sum += at_rest(DEEP, share).force.y;
        }
        return sum;
    };
    REQUIRE(total_lift(a) == Approx(total_lift(b)));
}

TEST_CASE("Authored points replace the derived ones", "[buoyancy]") {
    // A hull whose float points are not its corners: a deep keel, a bow that
    // lifts early.
    BuoyancyComponent b;
    b.points = {{2.0f, -0.3f, 0.0f}, {-2.0f, -0.3f, 0.0f}};
    const ColliderComponent c = box({0.5f, 0.5f, 0.5f});
    const BuoyancySamples s = buoyancy_samples(b, &c);

    REQUIRE(s.points.size() == 2);
    REQUIRE(s.points[0].x == Approx(2.0f));
    // The volume still comes from the collider: authoring where a hull floats
    // is not authoring how much water it pushes aside.
    REQUIRE(s.volume == Approx(1.0f));
}

TEST_CASE("A round body is sampled on its axes", "[buoyancy]") {
    BuoyancyComponent b;
    ColliderComponent c;
    c.shape = ColliderShape::Sphere;
    c.radius = 1.0f;
    const BuoyancySamples s = buoyancy_samples(b, &c);

    REQUIRE(s.points.size() == 6);
    REQUIRE(s.volume == Approx(4.18879f).margin(1e-4));
}

TEST_CASE("A body with no collider has no size to float with", "[buoyancy]") {
    // Guessing a volume would float it at a scale nothing else in the scene
    // agrees with, so it is better to do nothing and be visibly wrong.
    BuoyancyComponent b;
    const BuoyancySamples s = buoyancy_samples(b, nullptr);
    REQUIRE(s.points.empty());
    REQUIRE(s.volume == Approx(0.0f));
}

TEST_CASE("A crate dropped in water settles at its waterline", "[buoyancy]") {
    // The unit tests above each pin one term. This is the whole model doing
    // its job: integrate a real body falling into water and see where it
    // comes to rest. A force model can pass every term and still oscillate
    // forever or sink, and only running it shows that.
    ColliderComponent col;
    col.shape = ColliderShape::Box;
    col.half_extents = {0.5f, 0.5f, 0.5f};

    BuoyancyComponent b;
    b.density = 0.6f;
    b.drag = 1.2f;

    const BuoyancySamples s = buoyancy_samples(b, &col);
    const float share = s.volume / static_cast<float>(s.points.size());
    const float mass = s.volume * b.density * WATER_DENSITY;

    float y = 3.0f;
    float vy = 0.0f;
    constexpr float DT = 1.0f / 60.0f;

    for (int step = 0; step < 1800; ++step) {
        float force = -mass * GRAVITY;
        for (const auto& p : s.points) {
            const float depth = submersion_depth(y + p.y, 0.0f);
            force += buoyancy_at(depth, share, s.span, b.drag, glm::vec3(0.0f, vy, 0.0f),
                                 glm::vec3(0.0f), GRAVITY)
                         .force.y;
        }
        vy += (force / mass) * DT;
        y += vy * DT;
    }

    // Six tenths as dense as water, so it floats with six tenths of itself
    // under: the centre of a one-metre cube ends a tenth of a metre below the
    // surface. That it lands there is Archimedes; that it has stopped moving
    // is the drag.
    REQUIRE(y == Approx(-0.1f).margin(0.05f));
    REQUIRE(std::abs(vy) < 0.05f);
}

TEST_CASE("A stone dropped in water keeps going down", "[buoyancy]") {
    // Denser than water, so no depth exists where the water holds it up. A
    // spring-shaped model would find one and leave the stone hovering.
    ColliderComponent col;
    col.shape = ColliderShape::Box;
    col.half_extents = {0.5f, 0.5f, 0.5f};

    BuoyancyComponent b;
    b.density = 2.5f;
    b.drag = 1.2f;

    const BuoyancySamples s = buoyancy_samples(b, &col);
    const float share = s.volume / static_cast<float>(s.points.size());
    const float mass = s.volume * b.density * WATER_DENSITY;

    float y = 0.0f;
    float vy = 0.0f;
    constexpr float DT = 1.0f / 60.0f;
    for (int step = 0; step < 600; ++step) {
        float force = -mass * GRAVITY;
        for (const auto& p : s.points) {
            const float depth = submersion_depth(y + p.y, 0.0f);
            force += buoyancy_at(depth, share, s.span, b.drag, glm::vec3(0.0f, vy, 0.0f),
                                 glm::vec3(0.0f), GRAVITY)
                         .force.y;
        }
        vy += (force / mass) * DT;
        y += vy * DT;
    }
    REQUIRE(y < -5.0f);
    REQUIRE(vy < 0.0f);
}

TEST_CASE("A log floats lower than a cork", "[buoyancy]") {
    // Density decides the waterline, and it does so without appearing in the
    // force at all — it is the body's mass against the same lift.
    ColliderComponent col;
    col.shape = ColliderShape::Box;
    col.half_extents = {0.5f, 0.5f, 0.5f};

    auto settle = [&col](float density) {
        BuoyancyComponent b;
        b.density = density;
        b.drag = 1.5f;
        const BuoyancySamples s = buoyancy_samples(b, &col);
        const float share = s.volume / static_cast<float>(s.points.size());
        const float mass = s.volume * density * WATER_DENSITY;

        float y = 0.5f;
        float vy = 0.0f;
        constexpr float DT = 1.0f / 60.0f;
        for (int step = 0; step < 1800; ++step) {
            float force = -mass * GRAVITY;
            for (const auto& p : s.points) {
                const float depth = submersion_depth(y + p.y, 0.0f);
                force += buoyancy_at(depth, share, s.span, b.drag,
                                     glm::vec3(0.0f, vy, 0.0f), glm::vec3(0.0f), GRAVITY)
                             .force.y;
            }
            vy += (force / mass) * DT;
            y += vy * DT;
        }
        return y;
    };

    const float cork = settle(0.25f);
    const float log = settle(0.75f);
    REQUIRE(cork > log);
    // A quarter as dense floats a quarter under: centre a quarter of a metre
    // above the surface on a one-metre body.
    REQUIRE(cork == Approx(0.25f).margin(0.06f));
    REQUIRE(log == Approx(-0.25f).margin(0.06f));
}

TEST_CASE("A light body floats high, not just barely", "[buoyancy]") {
    // The case that sank in a real scene: a one-metre cube with the density
    // dial at 0.1. It floated for a moment and then went down forever,
    // because the rigidbody's authored mass of 1.0 was what the solver used
    // while the dial promised a tenth of water. The two are the same property
    // and the density is what drives the mass now, so this is what the dial
    // has to deliver.
    ColliderComponent col;
    col.shape = ColliderShape::Box;
    col.half_extents = {0.5f, 0.5f, 0.5f};

    BuoyancyComponent b;
    b.density = 0.1f;
    b.drag = 0.2f;

    const BuoyancySamples s = buoyancy_samples(b, &col);
    const float share = s.volume / static_cast<float>(s.points.size());
    const float mass = s.volume * b.density * WATER_DENSITY;

    float y = 4.0f;
    float vy = 0.0f;
    constexpr float DT = 1.0f / 60.0f;
    for (int step = 0; step < 3600; ++step) {
        float force = -mass * GRAVITY;
        for (const auto& p : s.points) {
            const float depth = submersion_depth(y + p.y, 0.0f);
            force += buoyancy_at(depth, share, s.span, b.drag, glm::vec3(0.0f, vy, 0.0f),
                                 glm::vec3(0.0f), GRAVITY)
                         .force.y;
        }
        vy += (force / mass) * DT;
        y += vy * DT;
    }

    // A tenth as dense as water sits with a tenth of itself under, so most of
    // the cube stands proud: its centre ends well above the waterline.
    REQUIRE(y == Approx(0.4f).margin(0.08f));
    // And it is resting there, not on its way past.
    REQUIRE(std::abs(vy) < 0.1f);
}
