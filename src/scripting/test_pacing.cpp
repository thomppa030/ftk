#include "scripting/test_pacing.hpp"

#include "physics/physics_types.hpp"
#include "physics/physics_world.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <format>
#include <limits>

namespace fjell {

namespace {

// What each of a node's recorded values is, as the report names it.
constexpr std::array<const char*, 17> FIELD_NAMES = {
    "position x", "position y", "position z",
    "rotation x", "rotation y", "rotation z", "rotation w",
    "scale x", "scale y", "scale z",
    "velocity x", "velocity y", "velocity z",
    "spin x", "spin y", "spin z",
    "ground state",
};

// Uneven frames: anywhere from 2 to 45 ms, and every fiftieth a quarter
// second hitch, which the clock cuts to the most steps a frame may run.
constexpr double SHORTEST_FRAME = 0.002;
constexpr double LONGEST_FRAME = 0.045;
constexpr int FRAMES_BETWEEN_HITCHES = 50;
constexpr double HITCH = 0.25;
constexpr std::mt19937::result_type JITTER_SEED = 20260925;

[[nodiscard]] bool same_bits(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

[[nodiscard]] bool moved(const Transform& from, const Transform& to) {
    return from.position != to.position || from.rotation != to.rotation || from.scale != to.scale;
}

} // namespace

Result<std::vector<Pacing>> parse_pacings(std::string_view list) {
    std::vector<Pacing> pacings;
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view item = list.substr(0, comma);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);

        Pacing pacing;
        if (item == "jitter") {
            pacing.name = "jitter";
        } else {
            double fps = 0.0;
            const auto [end, error] = std::from_chars(item.data(), item.data() + item.size(), fps);
            if (error != std::errc{} || end != item.data() + item.size() || fps < 1.0 || fps > 1000.0) {
                return make_error(std::format("--pacing: '{}' is not a frame rate from 1 to 1000 "
                                              "or 'jitter'",
                                              item));
            }
            pacing.fps = fps;
            pacing.name = std::format("{:g} fps", fps);
        }
        const bool repeated = std::any_of(pacings.begin(), pacings.end(),
                                          [&](const Pacing& p) { return p.name == pacing.name; });
        if (repeated) return make_error(std::format("--pacing: {} is listed twice", pacing.name));
        pacings.push_back(std::move(pacing));
    }
    if (pacings.size() < 2) {
        return make_error("--pacing needs at least two pacings to compare, as in 30,144,jitter");
    }
    return pacings;
}

PacedFrames::PacedFrames(const Pacing& pacing)
    : fps_(pacing.fps), rng_(JITTER_SEED), uneven_(SHORTEST_FRAME, LONGEST_FRAME) {}

double PacedFrames::next() {
    if (fps_ > 0.0) return 1.0 / fps_;
    ++frames_;
    if (frames_ % FRAMES_BETWEEN_HITCHES == 0) return HITCH;
    return uneven_(rng_);
}

void StepRecording::record(Scene& scene, const PhysicsWorld* physics) {
    Step& step = steps_.emplace_back();
    for (const auto& obj : scene.objects()) {
        const Node3D* spatial = obj->as_spatial();
        if (spatial == nullptr) continue;

        const auto* body = obj->components.get<RigidbodyComponent>();
        const auto* character = obj->components.get<CharacterComponent>();
        // A node spawned in this step has no start to compare with: it is
        // new, which is a change.
        const bool spawned = spatial->step_start_index_ == std::numeric_limits<std::uint64_t>::max();
        if (body == nullptr && character == nullptr && !spawned &&
            !moved(spatial->step_start_transform_, spatial->transform)) {
            continue;
        }

        NodeState state;
        state.id = obj->id;
        const Transform& t = spatial->transform;
        glm::vec3 velocity{0.0f};
        glm::vec3 spin{0.0f};
        float ground = 0.0f;
        if (body != nullptr && physics != nullptr) {
            velocity = physics->get_linear_velocity(obj->id);
            spin = physics->get_angular_velocity(obj->id);
        }
        if (character != nullptr) {
            velocity = character->velocity;
            ground = static_cast<float>(character->ground_state);
        }
        state.values = {
            t.position.x, t.position.y, t.position.z,
            t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
            t.scale.x, t.scale.y, t.scale.z,
            velocity.x, velocity.y, velocity.z,
            spin.x, spin.y, spin.z,
            ground,
        };
        step.push_back(state);
        names_.try_emplace(obj->id, obj->name);
    }
}

std::string StepRecording::name_of(uint32_t id) const {
    const auto it = names_.find(id);
    return it != names_.end() ? it->second : std::format("node {}", id);
}

std::optional<Divergence> StepRecording::first_divergence(const StepRecording& other) const {
    const std::size_t shared = std::min(steps_.size(), other.steps_.size());
    for (std::size_t s = 0; s < shared; ++s) {
        const Step& a = steps_[s];
        const Step& b = other.steps_[s];
        const std::size_t nodes = std::min(a.size(), b.size());
        for (std::size_t n = 0; n < nodes; ++n) {
            if (a[n].id != b[n].id) {
                // One run has a node the other does not have here: it moved,
                // spawned or was destroyed in one and not the other.
                const bool first_has = std::none_of(b.begin(), b.end(), [&](const NodeState& x) {
                    return x.id == a[n].id;
                });
                const uint32_t id = first_has ? a[n].id : b[n].id;
                return Divergence{.step = s,
                                  .node = name_of(id),
                                  .field = "moved or present",
                                  .first = first_has ? "yes" : "no",
                                  .second = first_has ? "no" : "yes"};
            }
            for (std::size_t v = 0; v < VALUE_COUNT; ++v) {
                if (same_bits(a[n].values[v], b[n].values[v])) continue;
                return Divergence{.step = s,
                                  .node = name_of(a[n].id),
                                  .field = FIELD_NAMES[v],
                                  .first = std::format("{}", a[n].values[v]),
                                  .second = std::format("{}", b[n].values[v])};
            }
        }
        if (a.size() != b.size()) {
            const bool first_longer = a.size() > b.size();
            const uint32_t id = first_longer ? a[nodes].id : b[nodes].id;
            return Divergence{.step = s,
                              .node = first_longer ? name_of(id) : other.name_of(id),
                              .field = "moved or present",
                              .first = first_longer ? "yes" : "no",
                              .second = first_longer ? "no" : "yes"};
        }
    }
    return std::nullopt;
}

} // namespace fjell
