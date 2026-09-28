#include "fjell/api.h"
#include "fjell/physics_api.hpp"
#include "physics/physics_types.hpp"
#include "physics/physics_world.hpp"
#include "scene/component_type.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "scripting/script_globals.hpp"

#include <algorithm>

namespace {

// Every entry point is a no-op outside play mode, where no physics world
// exists. A script that queries in the editor gets "nothing hit" rather than
// having to guard each call.
fjell::PhysicsWorld* world() {
    return fjell::script_services().physics;
}

fjell::QueryFilter make_filter(uint32_t channel_mask, uint32_t ignore_node) {
    fjell::QueryFilter filter;
    filter.channel_mask = channel_mask;
    if (ignore_node != 0) filter.ignore_nodes.push_back(ignore_node);
    return filter;
}

glm::vec3 to_vec3(const float* v) {
    return v != nullptr ? glm::vec3{v[0], v[1], v[2]} : glm::vec3{0.0F};
}

void write_vec3(const glm::vec3& v, float* out) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

// The node a script's RigidbodyComponent sits on, which the physics world
// knows its body by; 0 when it is no component in the scene.
uint32_t body_node(const void* body) {
    auto* scene = fjell::script_services().scene;
    if (scene == nullptr) return 0;
    const fjell::Node* owner =
        scene->owner_of(fjell::component_type_id<fjell::RigidbodyComponent>(), body);
    return owner != nullptr ? owner->id : 0;
}

void write_hit(const fjell::RaycastHit& hit, FjellRaycastHit* out) {
    out->position[0] = hit.position.x;
    out->position[1] = hit.position.y;
    out->position[2] = hit.position.z;
    out->normal[0] = hit.normal.x;
    out->normal[1] = hit.normal.y;
    out->normal[2] = hit.normal.z;
    out->distance = hit.distance;
    out->node_id = hit.object_id;
    out->surface = hit.surface;
}

fjell::QueryShape to_shape(uint32_t shape) {
    switch (shape) {
        case 1: return fjell::QueryShape::box;
        case 2: return fjell::QueryShape::capsule;
        default: return fjell::QueryShape::sphere;
    }
}

} // namespace

extern "C" FJELL_SDK_EXPORT int fjell_physics_raycast(
        const float* origin, const float* dir, float max_distance,
        uint32_t channel_mask, uint32_t ignore_node, FjellRaycastHit* out) {
    auto* physics = world();
    if (physics == nullptr || out == nullptr) return 0;

    fjell::RaycastHit hit;
    if (!physics->raycast(to_vec3(origin), to_vec3(dir), max_distance, hit,
                          make_filter(channel_mask, ignore_node))) {
        return 0;
    }
    write_hit(hit, out);
    return 1;
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_physics_raycast_all(
        const float* origin, const float* dir, float max_distance,
        uint32_t channel_mask, uint32_t ignore_node,
        FjellRaycastHit* out, uint32_t max_hits) {
    auto* physics = world();
    if (physics == nullptr || out == nullptr || max_hits == 0) return 0;

    auto hits = physics->raycast_all(to_vec3(origin), to_vec3(dir), max_distance,
                                     make_filter(channel_mask, ignore_node));
    auto count = static_cast<uint32_t>(std::min<size_t>(hits.size(), max_hits));
    for (uint32_t i = 0; i < count; ++i) write_hit(hits[i], &out[i]);
    return count;
}

extern "C" FJELL_SDK_EXPORT int fjell_physics_shape_cast(
        uint32_t shape, const float* extents, const float* origin, const float* dir,
        float max_distance, uint32_t channel_mask, uint32_t ignore_node,
        FjellRaycastHit* out) {
    auto* physics = world();
    if (physics == nullptr || out == nullptr) return 0;

    fjell::RaycastHit hit;
    if (!physics->shape_cast(to_shape(shape), to_vec3(extents), to_vec3(origin),
                             to_vec3(dir), max_distance, hit,
                             make_filter(channel_mask, ignore_node))) {
        return 0;
    }
    write_hit(hit, out);
    return 1;
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_physics_overlap(
        uint32_t shape, const float* extents, const float* position,
        uint32_t channel_mask, uint32_t ignore_node,
        uint32_t* out_nodes, uint32_t max_nodes) {
    auto* physics = world();
    if (physics == nullptr || out_nodes == nullptr || max_nodes == 0) return 0;

    auto nodes = physics->overlap(to_shape(shape), to_vec3(extents), to_vec3(position),
                                  make_filter(channel_mask, ignore_node));
    auto count = static_cast<uint32_t>(std::min<size_t>(nodes.size(), max_nodes));
    std::copy_n(nodes.begin(), count, out_nodes);
    return count;
}

// ── A body, through the RigidbodyComponent a script holds ───────────────

extern "C" FJELL_SDK_EXPORT void fjell_rigidbody_add_force(const void* body, const float* force) {
    if (auto* physics = world(); physics != nullptr) {
        if (const uint32_t node = body_node(body); node != 0) physics->apply_force(node, to_vec3(force));
    }
}

extern "C" FJELL_SDK_EXPORT void fjell_rigidbody_add_impulse(const void* body, const float* impulse) {
    if (auto* physics = world(); physics != nullptr) {
        if (const uint32_t node = body_node(body); node != 0) physics->apply_impulse(node, to_vec3(impulse));
    }
}

extern "C" FJELL_SDK_EXPORT void fjell_rigidbody_linear_velocity(const void* body, float* out) {
    auto* physics = world();
    const uint32_t node = physics != nullptr ? body_node(body) : 0;
    write_vec3(node != 0 ? physics->get_linear_velocity(node) : glm::vec3{0.0F}, out);
}

extern "C" FJELL_SDK_EXPORT void fjell_rigidbody_set_linear_velocity(const void* body, const float* velocity) {
    if (auto* physics = world(); physics != nullptr) {
        if (const uint32_t node = body_node(body); node != 0) physics->set_linear_velocity(node, to_vec3(velocity));
    }
}

extern "C" FJELL_SDK_EXPORT void fjell_rigidbody_angular_velocity(const void* body, float* out) {
    auto* physics = world();
    const uint32_t node = physics != nullptr ? body_node(body) : 0;
    write_vec3(node != 0 ? physics->get_angular_velocity(node) : glm::vec3{0.0F}, out);
}

extern "C" FJELL_SDK_EXPORT void fjell_rigidbody_set_angular_velocity(const void* body, const float* velocity) {
    if (auto* physics = world(); physics != nullptr) {
        if (const uint32_t node = body_node(body); node != 0) physics->set_angular_velocity(node, to_vec3(velocity));
    }
}

// ── The world ────────────────────────────────────────────────────────────

extern "C" FJELL_SDK_EXPORT void fjell_physics_set_gravity(const float* gravity) {
    if (auto* physics = world()) physics->set_gravity(to_vec3(gravity));
}

extern "C" FJELL_SDK_EXPORT void fjell_physics_get_gravity(float* out) {
    if (out == nullptr) return;
    auto* physics = world();
    auto g = physics != nullptr ? physics->gravity() : glm::vec3{0.0F};
    out[0] = g.x;
    out[1] = g.y;
    out[2] = g.z;
}
