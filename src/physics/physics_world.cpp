#include "physics/physics_world.hpp"
#include "physics/buoyancy.hpp"
#include "physics/collision_channel.hpp"
#include "physics/collision_shape.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "animation/animator_component.hpp"
#include "animation/skeleton.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "renderer/resources/model.hpp"
#include "scene/scene.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/PhysicsMaterialSimple.h>

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <cstdarg>
#include <mutex>
#include <algorithm>
#include <unordered_set>
#include <thread>

// Jolt requires a custom trace and assert implementation
static void jolt_trace(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    fjell::log::core()->info("[Jolt] {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
static bool jolt_assert_failed(const char* expression, const char* message,
                               const char* file, unsigned int line) {
    fjell::log::core()->error("[Jolt] Assert failed: {} : {} ({}:{})",
                              expression, message ? message : "", file, line);
    return true; // break into debugger
}
#endif

namespace fjell {

// ── Broadphase layers ───────────────────────────────────────────────────

namespace layers {
static constexpr JPH::ObjectLayer STATIC = 0;
static constexpr JPH::ObjectLayer MOVING = 1;
static constexpr JPH::ObjectLayer SENSOR = 2;
// Filtered bodies get one layer each from here up, one per distinct filter.
static constexpr JPH::ObjectLayer FIRST_FILTERED = 3;
static constexpr JPH::uint NUM_LAYERS = 256;
} // namespace layers

namespace broad_phase {
static constexpr JPH::BroadPhaseLayer STATIC(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr JPH::BroadPhaseLayer SENSOR(2);
static constexpr JPH::uint NUM_LAYERS = 3;
} // namespace broad_phase

// Jolt asks about pairs by object layer, so each distinct CollisionFilter in
// the scene is registered as one layer and the pair filter looks both sides up
// here. The rule itself stays in collision_channel.cpp: this translates, it
// does not decide.
class FilterTable {
public:
    // Layers 0-2 keep their meaning for bodies with no filter of their own
    // (terrain), so the broadphase mapping below stays valid for them.
    // Whether a body moves is a property of the body, not of its channel: a
    // dynamic prop and the level geometry it lands on can share a channel and
    // still need different broadphase treatment. Layers key on both so the
    // spatial split stays correct.
    struct Entry {
        CollisionFilter filter;
        bool movable;
    };

    [[nodiscard]] JPH::ObjectLayer layer_for(const CollisionFilter& filter, bool movable) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].movable == movable &&
                entries_[i].filter.channel == filter.channel &&
                entries_[i].filter.block_mask == filter.block_mask &&
                entries_[i].filter.overlap_mask == filter.overlap_mask) {
                return static_cast<JPH::ObjectLayer>(layers::FIRST_FILTERED + i);
            }
        }
        entries_.push_back({filter, movable});
        return static_cast<JPH::ObjectLayer>(layers::FIRST_FILTERED + entries_.size() - 1);
    }

    [[nodiscard]] const CollisionFilter* find(JPH::ObjectLayer layer) const {
        const auto* entry = find_entry(layer);
        return entry != nullptr ? &entry->filter : nullptr;
    }

    [[nodiscard]] const Entry* find_entry(JPH::ObjectLayer layer) const {
        if (layer < layers::FIRST_FILTERED) return nullptr;
        size_t index = layer - layers::FIRST_FILTERED;
        return index < entries_.size() ? &entries_[index] : nullptr;
    }

    void clear() { entries_.clear(); }

private:
    std::vector<Entry> entries_;
};

class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override {
        return broad_phase::NUM_LAYERS;
    }
    // Filtered layers carry their own channel, but the broadphase split is a
    // spatial optimization rather than a gameplay concept, so it only needs to
    // know whether a body is expected to move.
    const FilterTable* table{nullptr};

    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        if (const auto* entry = table != nullptr ? table->find_entry(layer) : nullptr) {
            if (entry->filter.channel == CollisionChannel::trigger) return broad_phase::SENSOR;
            return entry->movable ? broad_phase::MOVING : broad_phase::STATIC;
        }
        switch (layer) {
            case layers::STATIC: return broad_phase::STATIC;
            case layers::SENSOR: return broad_phase::SENSOR;
            default: return broad_phase::MOVING;
        }
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    // Jolt only declares the name hook when its profiler is compiled in.
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        if (layer == broad_phase::STATIC) return "STATIC";
        if (layer == broad_phase::MOVING) return "MOVING";
        if (layer == broad_phase::SENSOR) return "SENSOR";
        return "UNKNOWN";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    const FilterTable* table{nullptr};

    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer obj_layer,
                                     JPH::BroadPhaseLayer bp_layer) const override {
        // A filtered body's own table decides; the broadphase only prunes the
        // pair of static groups that can never move into each other.
        if (const auto* entry = table != nullptr ? table->find_entry(obj_layer) : nullptr) {
            // Two bodies that cannot move can never come to touch, which is the
            // one pair the broadphase is free to drop. Everything else is the
            // channel table's decision, not this one's.
            if (!entry->movable) return bp_layer != broad_phase::STATIC;
            return true;
        }
        // Static vs static = no collision
        if (obj_layer == layers::STATIC)
            return bp_layer != broad_phase::STATIC;
        // Sensors collide with everything (for contact callbacks)
        if (obj_layer == layers::SENSOR)
            return true;
        // Moving collides with everything
        return true;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    const FilterTable* table{nullptr};

    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        const auto* fa = table != nullptr ? table->find(a) : nullptr;
        const auto* fb = table != nullptr ? table->find(b) : nullptr;
        if (fa != nullptr && fb != nullptr) {
            // Overlap still needs the pair reported, so only ignore drops it.
            return collision_response(*fa, *fb) != CollisionResponse::ignore;
        }

        // Unfiltered bodies keep the original coarse rule.
        if (a == layers::STATIC && b == layers::STATIC) return false;
        return true;
    }
};

// ── Helpers ─────────────────────────────────────────────────────────────

static JPH::Vec3 to_jolt(const glm::vec3& v) {
    return JPH::Vec3(v.x, v.y, v.z);
}

static glm::vec3 to_glm(JPH::Vec3 v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}

static JPH::Quat to_jolt_quat(const glm::quat& q) {
    // Guard against NaN/zero/denormalized quaternions — Jolt asserts IsNormalized()
    if (std::isnan(q.x) || std::isnan(q.y) || std::isnan(q.z) || std::isnan(q.w)) {
        return JPH::Quat::sIdentity();
    }
    float len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (len2 < 1e-8f) return JPH::Quat::sIdentity();
    // Always normalize to satisfy Jolt's strict tolerance
    float inv_len = 1.0f / std::sqrt(len2);
    return JPH::Quat(q.x * inv_len, q.y * inv_len, q.z * inv_len, q.w * inv_len);
}

static glm::quat to_glm_quat(JPH::Quat q) {
    return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
}

// ── Contact listener ────────────────────────────────────────────────────

class FjellContactListener final : public JPH::ContactListener {
public:
    // Back-pointer for resolving body → object ID
    std::unordered_map<uint32_t, uint32_t>* body_to_object{nullptr};
    // Back-pointer for resolving a shape's material → surface id
    const std::unordered_map<const JPH::PhysicsMaterial*, uint32_t>* material_surfaces{nullptr};

    // Bodies that stand in for a character. Their contacts are not reported
    // from here: a character sweeps its own shape and knows every contact it
    // made, sensors and other characters included, and reports from that
    // list. Reporting the body's pairs as well would double every event.
    std::unordered_set<uint32_t> character_bodies;

    // Events collected this frame (swapped out at clear())
    std::vector<std::pair<uint32_t, CollisionEvent>> collision_enter_events;
    std::vector<std::pair<uint32_t, CollisionEvent>> collision_exit_events;
    std::vector<std::pair<uint32_t, CollisionEvent>> trigger_enter_events;
    std::vector<std::pair<uint32_t, CollisionEvent>> trigger_exit_events;

    void clear() {
        collision_enter_events.clear();
        collision_exit_events.clear();
        trigger_enter_events.clear();
        trigger_exit_events.clear();
    }

    JPH::ValidateResult OnContactValidate(
        [[maybe_unused]] const JPH::Body& body1,
        [[maybe_unused]] const JPH::Body& body2,
        [[maybe_unused]] JPH::RVec3Arg base_offset,
        [[maybe_unused]] const JPH::CollideShapeResult& collision_result) override {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(
        const JPH::Body& body1, const JPH::Body& body2,
        const JPH::ContactManifold& manifold,
        [[maybe_unused]] JPH::ContactSettings& settings) override {

        uint32_t key1 = body1.GetID().GetIndexAndSequenceNumber();
        uint32_t key2 = body2.GetID().GetIndexAndSequenceNumber();
        if (character_bodies.contains(key1) || character_bodies.contains(key2)) return;

        auto it1 = body_to_object->find(key1);
        auto it2 = body_to_object->find(key2);
        if (it1 == body_to_object->end() || it2 == body_to_object->end()) return;

        uint32_t obj1 = it1->second;
        uint32_t obj2 = it2->second;

        glm::vec3 contact_point{0.0f};
        if (manifold.mRelativeContactPointsOn1.size() > 0) {
            contact_point = to_glm(manifold.GetWorldSpaceContactPointOn1(0));
        }
        auto contact_normal = to_glm(manifold.mWorldSpaceNormal);

        // Each side learns what the other is made of where they touch.
        uint32_t surface1 = 0;
        uint32_t surface2 = 0;
        if (material_surfaces != nullptr) {
            auto lookup = [&](const JPH::PhysicsMaterial* material) {
                auto found = material_surfaces->find(material);
                return found != material_surfaces->end() ? found->second : 0U;
            };
            surface1 = lookup(body1.GetShape()->GetMaterial(manifold.mSubShapeID1));
            surface2 = lookup(body2.GetShape()->GetMaterial(manifold.mSubShapeID2));
        }

        bool is_sensor = body1.IsSensor() || body2.IsSensor();

        // Cache this contact for exit tracking
        uint64_t pair_key = (static_cast<uint64_t>(key1) << 32) | key2;
        {
            std::lock_guard lock(contact_mutex_);
            active_contacts_[pair_key] = {obj1, obj2, is_sensor};
        }

        if (is_sensor) {
            std::lock_guard lock(event_mutex_);
            trigger_enter_events.push_back({obj1, {obj2, contact_point, contact_normal, surface2}});
            trigger_enter_events.push_back({obj2, {obj1, contact_point, -contact_normal, surface1}});
        } else {
            std::lock_guard lock(event_mutex_);
            collision_enter_events.push_back({obj1, {obj2, contact_point, contact_normal, surface2}});
            collision_enter_events.push_back({obj2, {obj1, contact_point, -contact_normal, surface1}});
        }
    }

    void OnContactRemoved(
        const JPH::SubShapeIDPair& pair) override {

        uint32_t key1 = pair.GetBody1ID().GetIndexAndSequenceNumber();
        uint32_t key2 = pair.GetBody2ID().GetIndexAndSequenceNumber();
        uint64_t pair_key = (static_cast<uint64_t>(key1) << 32) | key2;

        CachedContact cached;
        {
            std::lock_guard lock(contact_mutex_);
            auto it = active_contacts_.find(pair_key);
            if (it == active_contacts_.end()) return;
            cached = it->second;
            active_contacts_.erase(it);
        }

        CollisionEvent exit_event{0, glm::vec3(0.0f), glm::vec3(0.0f), 0};

        if (cached.is_sensor) {
            std::lock_guard lock(event_mutex_);
            exit_event.other_id = cached.obj_id_2;
            trigger_exit_events.push_back({cached.obj_id_1, exit_event});
            exit_event.other_id = cached.obj_id_1;
            trigger_exit_events.push_back({cached.obj_id_2, exit_event});
        } else {
            std::lock_guard lock(event_mutex_);
            exit_event.other_id = cached.obj_id_2;
            collision_exit_events.push_back({cached.obj_id_1, exit_event});
            exit_event.other_id = cached.obj_id_1;
            collision_exit_events.push_back({cached.obj_id_2, exit_event});
        }
    }

private:
    struct CachedContact {
        uint32_t obj_id_1;
        uint32_t obj_id_2;
        bool is_sensor;
    };

    std::mutex contact_mutex_;
    std::mutex event_mutex_;
    std::unordered_map<uint64_t, CachedContact> active_contacts_;
};

// ── Impl ────────────────────────────────────────────────────────────────

// Jolt fixes the supporting volume at construction, but it depends on the
// capsule, and the capsule changes when a character crouches or is rescaled.
// The member is protected rather than private, so this is the one place the
// engine reaches into a Jolt object.
class FjellCharacter final : public JPH::CharacterVirtual {
public:
    using JPH::CharacterVirtual::CharacterVirtual;
    void set_supporting_volume(const JPH::Plane& plane) { mSupportingVolume = plane; }
};

// Only contacts on the lower hemisphere count as ground. Without this the
// capsule's side does too, and a character hanging off a ledge by its waist
// reads as standing on it.
static JPH::Plane supporting_volume_for(const CharacterCapsule& capsule) {
    float bottom_cap_center = capsule.center.y - capsule.height * 0.5f;
    return JPH::Plane(JPH::Vec3::sAxisY(), -bottom_cap_center);
}

struct CharacterEntry {
    JPH::Ref<FjellCharacter> character;
    uint32_t object_id;
    // The layer the character's filter maps to; both its capsule and the
    // sweep that moves it collide as this.
    JPH::ObjectLayer layer;
    // What the character was touching after its last update, so the next
    // update can tell a new contact from one that ended.
    struct Contact {
        uint32_t other_id;
        bool sensor;
    };
    std::vector<Contact> contacts;
    // The authored values the character was built from, so a change made in
    // the inspector or by a script — a crouch, a rescale, a channel switch —
    // is noticed and applied at the next step.
    struct Built {
        CharacterCapsule capsule;
        glm::vec3 scale;
        float max_slope_angle;
        float mass;
        CollisionFilter filter;
    };
    Built built;
    // World position last agreed between the node and the character. The node
    // sitting anywhere else at the next step means something outside physics
    // moved it — a teleport, a parent, the network — and the character follows.
    glm::vec3 synced_position{0.0f};
};


struct PhysicsWorld::Impl {
    std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system;
    std::unique_ptr<JPH::PhysicsSystem> physics_system;

    FilterTable filter_table;
    BPLayerInterface bp_layer_interface;
    ObjectVsBroadPhaseFilter obj_vs_bp_filter;
    ObjectLayerPairFilter obj_pair_filter;
    FjellContactListener contact_listener;

    // Bidirectional maps: Fjell object_id <-> Jolt BodyID (stored as uint32)
    std::unordered_map<uint32_t, JPH::BodyID> object_to_body;
    std::unordered_map<uint32_t, uint32_t> body_to_object; // body.GetIndexAndSequenceNumber() -> object_id

    // Character controllers
    std::vector<CharacterEntry> characters;

    // Bodies that follow an animated bone. One node owns several, so these sit
    // beside object_to_body rather than in it; body_to_object still maps each
    // back to the owning node, so a hit on a forearm reports the character.
    struct BoneBody {
        uint32_t object_id;
        int32_t bone;
        JPH::BodyID body;
    };
    std::vector<BoneBody> bone_bodies;

    // A shape bakes the node's scale when it is created and cannot be
    // rescaled afterwards, so the scale each body was built at is kept to
    // notice when it no longer matches and the shape must be rebuilt.
    std::unordered_map<uint32_t, glm::vec3> body_scales;

    // One Jolt material per surface id, created on first use and kept for
    // the world's lifetime, and the reverse map that turns the material a
    // shape reports back into the surface id gameplay compares. Jolt's own
    // material types carry only a debug name, which is all a surface needs
    // on that side; the identity lives in these maps.
    std::unordered_map<uint32_t, JPH::RefConst<JPH::PhysicsMaterial>> surface_materials;
    std::unordered_map<const JPH::PhysicsMaterial*, uint32_t> material_surfaces;

    // Null for surface 0, which is what a shape with no material means.
    const JPH::PhysicsMaterial* material_for(uint32_t surface) {
        if (surface == 0) return nullptr;
        auto it = surface_materials.find(surface);
        if (it != surface_materials.end()) return it->second.GetPtr();
        JPH::RefConst<JPH::PhysicsMaterial> material =
            new JPH::PhysicsMaterialSimple("surface " + std::to_string(surface), JPH::Color::sGrey);
        material_surfaces[material.GetPtr()] = surface;
        surface_materials[surface] = material;
        return material.GetPtr();
    }

    [[nodiscard]] uint32_t surface_of(const JPH::PhysicsMaterial* material) const {
        auto it = material_surfaces.find(material);
        return it != material_surfaces.end() ? it->second : 0;
    }
};

static bool s_jolt_initialized = false;

static void ensure_jolt_init() {
    if (s_jolt_initialized) return;

    JPH::RegisterDefaultAllocator();
    JPH::Trace = jolt_trace;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = jolt_assert_failed;
#endif
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    s_jolt_initialized = true;
    FJELL_CORE_INFO("Jolt Physics initialized");
}

PhysicsWorld::PhysicsWorld() : impl_(std::make_unique<Impl>()) {
    ensure_jolt_init();

    impl_->temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

    auto num_threads = std::max(1u, std::thread::hardware_concurrency() - 1);
    impl_->job_system = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, static_cast<int>(num_threads));

    constexpr JPH::uint max_bodies = 4096;
    constexpr JPH::uint num_body_mutexes = 0; // auto
    constexpr JPH::uint max_body_pairs = 4096;
    constexpr JPH::uint max_contact_constraints = 2048;

    impl_->bp_layer_interface.table = &impl_->filter_table;
    impl_->obj_vs_bp_filter.table = &impl_->filter_table;
    impl_->obj_pair_filter.table = &impl_->filter_table;

    impl_->physics_system = std::make_unique<JPH::PhysicsSystem>();
    impl_->physics_system->Init(
        max_bodies, num_body_mutexes, max_body_pairs, max_contact_constraints,
        impl_->bp_layer_interface, impl_->obj_vs_bp_filter, impl_->obj_pair_filter);

    impl_->physics_system->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

    // Wire up contact listener
    impl_->contact_listener.body_to_object = &impl_->body_to_object;
    impl_->contact_listener.material_surfaces = &impl_->material_surfaces;
    impl_->physics_system->SetContactListener(&impl_->contact_listener);
}

PhysicsWorld::~PhysicsWorld() {
    if (!impl_) return;

    // Remove all bodies
    auto& bi = impl_->physics_system->GetBodyInterface();
    for (auto& [obj_id, body_id] : impl_->object_to_body) {
        bi.RemoveBody(body_id);
        bi.DestroyBody(body_id);
    }
}

// ── Validation helpers ───────────────────────────────────────────────────

static constexpr float MIN_DIMENSION = 0.001f;

// A box half-extent below the backend's convex radius trips an assert rather
// than producing a degenerate shape, so a collider authored thinner than this
// is widened instead of taking the process down with it.
static constexpr float MIN_BOX_HALF_EXTENT = JPH::cDefaultConvexRadius;

static glm::vec3 clamp_box_half_extents(const glm::vec3& v) {
    return {std::max(v.x, MIN_BOX_HALF_EXTENT),
            std::max(v.y, MIN_BOX_HALF_EXTENT),
            std::max(v.z, MIN_BOX_HALF_EXTENT)};
}

static float clamp_positive(float v) {
    return std::max(v, MIN_DIMENSION);
}

// ── Collision primitives ────────────────────────────────────────────────

// Builds one authored collision shape. Geometry kinds read the ranges the
// primitive names out of the mesh's collision arrays; primitive kinds need no
// geometry at all. `scale` is baked in, since a shape cannot be rescaled after
// creation.
static JPH::ShapeRefC build_primitive_shape(const CollisionPrimitive& prim,
                                            const CollisionMesh& geometry,
                                            const glm::vec3& scale,
                                            const std::string& owner,
                                            const JPH::PhysicsMaterial* material) {
    switch (prim.kind) {
        case CollisionPrimitiveKind::box: {
            auto he = clamp_box_half_extents(prim.extents * scale);
            return new JPH::BoxShape(JPH::Vec3(he.x, he.y, he.z), JPH::cDefaultConvexRadius,
                                     material);
        }
        case CollisionPrimitiveKind::sphere: {
            float r = clamp_positive(prim.extents.x * std::max({scale.x, scale.y, scale.z}));
            return new JPH::SphereShape(r, material);
        }
        case CollisionPrimitiveKind::capsule: {
            float r = clamp_positive(prim.extents.x * std::max(scale.x, scale.z));
            float half_h = clamp_positive(prim.extents.y * 0.5f * scale.y);
            return new JPH::CapsuleShape(half_h, r, material);
        }
        case CollisionPrimitiveKind::convex_hull: {
            if (geometry.vertices == nullptr) return {};
            const auto& verts = *geometry.vertices;
            JPH::Array<JPH::Vec3> points;
            points.reserve(prim.vertex_count);
            for (uint32_t i = 0; i < prim.vertex_count; ++i) {
                uint32_t idx = prim.first_vertex + i;
                if (idx >= verts.size()) break;
                auto p = verts[idx] * scale;
                points.push_back(JPH::Vec3(p.x, p.y, p.z));
            }
            auto result = JPH::ConvexHullShapeSettings(points, JPH::cDefaultConvexRadius, material)
                              .Create();
            if (result.HasError()) {
                FJELL_CORE_ERROR("Failed to create hull collider for '{}': {}",
                                 owner, result.GetError().c_str());
                return {};
            }
            return result.Get();
        }
        case CollisionPrimitiveKind::triangle_mesh: {
            if (geometry.vertices == nullptr || geometry.indices == nullptr) return {};
            const auto& verts = *geometry.vertices;
            const auto& idxs = *geometry.indices;
            JPH::TriangleList triangles;
            triangles.reserve(prim.index_count / 3);
            for (uint32_t i = 0; i + 2 < prim.index_count; i += 3) {
                uint32_t base = prim.first_index + i;
                if (base + 2 >= idxs.size()) break;
                auto v0 = verts[idxs[base]] * scale;
                auto v1 = verts[idxs[base + 1]] * scale;
                auto v2 = verts[idxs[base + 2]] * scale;
                triangles.push_back(JPH::Triangle(
                    JPH::Float3(v0.x, v0.y, v0.z),
                    JPH::Float3(v1.x, v1.y, v1.z),
                    JPH::Float3(v2.x, v2.y, v2.z)));
            }
            // Every triangle indexes material 0; with no surface the list stays
            // empty and Jolt reports its default.
            JPH::PhysicsMaterialList materials;
            if (material != nullptr) materials.push_back(material);
            auto result = JPH::MeshShapeSettings(triangles, std::move(materials)).Create();
            if (result.HasError()) {
                FJELL_CORE_ERROR("Failed to create mesh collider for '{}': {}",
                                 owner, result.GetError().c_str());
                return {};
            }
            return result.Get();
        }
    }
    return {};
}

// Builds the shape a mesh's authored collision list describes. A single
// primitive at the origin becomes that shape directly; anything else becomes a
// compound, which is what lets several hulls approximate a concave object.
static JPH::ShapeRefC build_authored_shape(const std::vector<CollisionPrimitive>& primitives,
                                           const CollisionMesh& geometry,
                                           const glm::vec3& scale,
                                           const std::string& owner,
                                           const JPH::PhysicsMaterial* material,
                                           bool& out_supports_dynamic) {
    out_supports_dynamic = true;
    for (const auto& prim : primitives) {
        if (!prim.is_convex()) out_supports_dynamic = false;
    }

    if (primitives.size() == 1) {
        const auto& prim = primitives.front();
        auto shape = build_primitive_shape(prim, geometry, scale, owner, material);
        if (!shape) return {};
        // A placed shape needs the transform wrapper; an unplaced one does not.
        if (glm::length(prim.position) < 1e-5f &&
            std::abs(prim.rotation.w - 1.0f) < 1e-5f) {
            return shape;
        }
        JPH::StaticCompoundShapeSettings settings;
        settings.AddShape(to_jolt(prim.position * scale), to_jolt_quat(prim.rotation), shape);
        auto result = settings.Create();
        return result.HasError() ? JPH::ShapeRefC{} : result.Get();
    }

    JPH::StaticCompoundShapeSettings settings;
    uint32_t added = 0;
    for (const auto& prim : primitives) {
        auto shape = build_primitive_shape(prim, geometry, scale, owner, material);
        if (!shape) continue;
        settings.AddShape(to_jolt(prim.position * scale), to_jolt_quat(prim.rotation), shape);
        ++added;
    }
    if (added == 0) return {};

    auto result = settings.Create();
    if (result.HasError()) {
        FJELL_CORE_ERROR("Failed to create compound collider for '{}': {}",
                         owner, result.GetError().c_str());
        return {};
    }
    return result.Get();
}

// ── Terrain ─────────────────────────────────────────────────────────────

// Builds the static body for a terrain node. Heights are stored normalized, so
// the scale carries `height_scale` and the offset carries `height_offset` —
// the same reconstruction the terrain shader performs, expressed as Jolt's
// surface equation rather than repeated per sample.
static JPH::BodyID create_terrain_body(JPH::BodyInterface& bi, FilterTable& filters,
                                       PhysicsWorld::Impl& impl, const TerrainAsset& terrain,
                                       const glm::vec3& world_position) {
    const auto& data = terrain.data();
    const uint32_t samples = data.sample_count();

    // Jolt subdivides the field into blocks and needs at least two per side.
    if (samples < 2 || data.heights.size() < static_cast<size_t>(samples) * samples) {
        return {};
    }

    // Sample spacing on X and Z. A terrain's size is its asset's `world_size`
    // and its shape is a grid sampled along world X and Z, so the node places
    // it and nothing more: rotating or scaling the node is ignored here
    // exactly as the renderer ignores it, rather than collision standing
    // somewhere the ground is not drawn.
    //
    // Jolt grows the field from the shape origin, so the min corner is offset
    // to put the node's pivot at the centre, as the renderer does.
    const float spacing = data.header.world_size / static_cast<float>(samples - 1);
    const glm::vec3 corner = terrain.local_min_corner();

    JPH::HeightFieldShapeSettings settings(
        data.heights.data(),
        JPH::Vec3(corner.x, data.header.height_offset, corner.z),
        JPH::Vec3(spacing, data.header.height_scale, spacing),
        samples);

    // Heights are normalized, so the format's full range is [0,1] whatever the
    // loaded data happens to span. Reserve all of it: SetHeights clamps to the
    // range fixed here, and sculpting terrain above its original peak would
    // otherwise flatten against a ceiling derived from the initial samples.
    settings.mMinHeightValue = 0.0f;
    settings.mMaxHeightValue = 1.0f;

    // One material per layer, indexed per quad by whichever layer dominates
    // the painted weights there. Only worth building when some layer has a
    // surface; otherwise the field reports Jolt's default everywhere.
    bool any_surface = false;
    for (uint32_t i = 0; i < terrain.layer_count(); ++i) {
        any_surface = any_surface || terrain.layer_surface(i) != 0;
    }
    if (any_surface) {
        for (uint32_t i = 0; i < FJTERRAIN_MAX_LAYERS; ++i) {
            const JPH::PhysicsMaterial* material = impl.material_for(terrain.layer_surface(i));
            settings.mMaterials.push_back(material != nullptr
                                              ? JPH::RefConst<JPH::PhysicsMaterial>(material)
                                              : JPH::PhysicsMaterial::sDefault);
        }
        const uint32_t quads = samples - 1;
        settings.mMaterialIndices.resize(static_cast<size_t>(quads) * quads);
        terrain.dominant_layers(0, 0, quads, quads, settings.mMaterialIndices.data());
    }

    auto shape_result = settings.Create();
    if (shape_result.HasError()) {
        FJELL_CORE_ERROR("Failed to create terrain collision shape: {}",
                         shape_result.GetError().c_str());
        return {};
    }

    // Terrain is world geometry, so it answers the channel table like any
    // other collider — a projectile set to ignore the world has to pass
    // through the ground too, not just through walls.
    auto layer = filters.layer_for(default_filter_for(CollisionChannel::world_static),
                                   false);

    JPH::BodyCreationSettings body_settings(
        shape_result.Get(),
        to_jolt(world_position),
        JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, layer);

    return bi.CreateAndAddBody(body_settings, JPH::EActivation::DontActivate);
}

// ── Bone colliders ──────────────────────────────────────────────────────

// One kinematic body per shape the skeleton authors. Kinematic because the
// animation drives them: the solver reads where they are, it does not decide.
// Each is registered against the owning node, so a query that hits a forearm
// reports the character rather than a body nothing else knows about.
void PhysicsWorld::create_bone_bodies(Node& node, const Skeleton& skeleton) {
    const auto& colliders = skeleton.colliders();
    if (colliders.empty()) return;

    auto* spatial = node.as_spatial();
    if (spatial == nullptr) return;

    const auto* col_comp = node.components.get<ColliderComponent>();
    auto filter = col_comp != nullptr ? col_comp->filter
                                      : default_filter_for(CollisionChannel::pawn);
    auto layer = impl_->filter_table.layer_for(filter, true);

    auto& bi = impl_->physics_system->GetBodyInterface();
    CollisionMesh no_geometry;

    // Bone shapes are the character's hit surfaces; they report the node's
    // collider surface, when it has one, so a hit on a forearm says what the
    // character is made of.
    const auto* node_collider = node.components.get<ColliderComponent>();
    const JPH::PhysicsMaterial* bone_material =
        impl_->material_for(node_collider != nullptr ? node_collider->surface : 0);

    for (const auto& col : colliders) {
        if (col.shape.bone < 0) continue;

        auto shape = build_primitive_shape(col.shape, no_geometry,
                                           spatial->transform.scale, node.name, bone_material);
        if (!shape) continue;

        // Placed at the node for now; step() moves it onto its bone before the
        // first solve, so the initial position only has to be valid.
        JPH::BodyCreationSettings settings(
            shape, to_jolt(spatial->transform.position),
            to_jolt_quat(spatial->transform.rotation),
            JPH::EMotionType::Kinematic, layer);
        settings.mCollideKinematicVsNonDynamic = true;

        auto body_id = bi.CreateAndAddBody(settings, JPH::EActivation::Activate);
        if (body_id.IsInvalid()) continue;

        impl_->bone_bodies.push_back({node.id, col.shape.bone, body_id});
        impl_->body_to_object[body_id.GetIndexAndSequenceNumber()] = node.id;
    }
}

// ── Body creation ───────────────────────────────────────────────────────

// Creates the body for one node, if it should have one. Split out of the
// play-start loop so a node spawned mid-play gets the same treatment as one
// that was there from the beginning — the alternative is objects that look
// solid and fall through the world.
// A node's world placement from its current local transforms, split into
// what a body needs. Scale is dropped from the rotation; shapes bake it.
static void world_placement(const Scene& scene, const Node3D& node,
                            glm::vec3& out_position, glm::quat& out_rotation) {
    glm::mat4 world = scene.current_world_matrix(node);
    out_position = glm::vec3{world[3]};
    glm::mat3 basis{glm::normalize(glm::vec3{world[0]}),
                    glm::normalize(glm::vec3{world[1]}),
                    glm::normalize(glm::vec3{world[2]})};
    out_rotation = glm::quat_cast(basis);
}

void PhysicsWorld::create_body(Scene& scene, Node& node) {
    auto& bi = impl_->physics_system->GetBodyInterface();

    // A node that already has one is being rebuilt rather than added.
    destroy_body(node.id);

    auto* spatial = node.as_spatial();
    if (spatial == nullptr) return;

    // Character controller — handled in Step 5 section below
    auto* char_comp = node.components.get<CharacterComponent>();
    if (char_comp) {
        auto capsule = scaled_capsule(*char_comp, spatial->transform.scale);
        capsule.radius = clamp_positive(capsule.radius);
        capsule.height = clamp_positive(capsule.height);

        JPH::CharacterVirtualSettings settings;
        settings.mShape = new JPH::CapsuleShape(capsule.height * 0.5f, capsule.radius);
        settings.mShapeOffset = to_jolt(capsule.center);
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(char_comp->max_slope_angle);
        settings.mMass = std::max(char_comp->mass, 1.0f);
        // Pushing a dynamic body is bounded by roughly the character's own
        // weight, so a heavy character shoves a crate and a light one is
        // stopped by it. Unbounded, the default is a fixed 100 N for everyone.
        settings.mMaxStrength = settings.mMass * impl_->physics_system->GetGravity().Length();
        settings.mUp = JPH::Vec3(0, 1, 0);
        settings.mSupportingVolume = supporting_volume_for(capsule);
        // Mesh colliders have internal edges between their triangles; without
        // this the capsule catches on them while crossing a flat floor.
        settings.mEnhancedInternalEdgeRemoval = true;
        JPH::ObjectLayer layer = impl_->filter_table.layer_for(char_comp->filter, true);
        settings.mInnerBodyShape = new JPH::CapsuleShape(capsule.height * 0.5f, capsule.radius);
        settings.mInnerBodyLayer = layer;

        glm::vec3 world_position;
        glm::quat world_rotation;
        world_placement(scene, *spatial, world_position, world_rotation);

        auto character = new FjellCharacter(
            &settings,
            to_jolt(world_position),
            to_jolt_quat(world_rotation),
            0, // user data
            impl_->physics_system.get());

        CharacterEntry::Built built{capsule, spatial->transform.scale,
                                    char_comp->max_slope_angle, char_comp->mass,
                                    char_comp->filter};
        impl_->characters.push_back({character, node.id, layer, {}, built, world_position});

        // The inner body is what rays and other bodies find; contacts against
        // it resolve to this node, and its events come from the character.
        auto inner_id = character->GetInnerBodyID();
        if (!inner_id.IsInvalid()) {
            impl_->body_to_object[inner_id.GetIndexAndSequenceNumber()] = node.id;
            impl_->contact_listener.character_bodies.insert(inner_id.GetIndexAndSequenceNumber());
        }
        return;
    }

    // Terrain is solid by virtue of being terrain — no collider component,
    // the way landscapes work in every engine that has them. The heights
    // the shader draws from are the heights bodies collide against, so the
    // surface can never disagree with what is on screen.
    if (const auto* terrain_comp = node.components.get<TerrainComponent>();
        terrain_comp != nullptr && terrain_comp->terrain != nullptr) {
        glm::vec3 world_position;
        glm::quat world_rotation;
        world_placement(scene, *spatial, world_position, world_rotation);
        if (auto body_id = create_terrain_body(bi, impl_->filter_table, *impl_,
                                               *terrain_comp->terrain, world_position);
            !body_id.IsInvalid()) {
            impl_->object_to_body[node.id] = body_id;
            impl_->body_to_object[body_id.GetIndexAndSequenceNumber()] = node.id;
        } else {
            FJELL_CORE_ERROR("Failed to create terrain body for '{}'", node.name);
        }
        return;
    }

    // Bone colliders: a skeletal mesh whose skeleton carries authored shapes
    // gets one kinematic body per shape, moved to its bone every frame. They
    // are independent of the node's own collider, so a character can have both
    // a capsule for movement and per-bone shapes for hit detection.
    if (const auto* skel_mesh = node.components.get<SkeletalMeshComponent>();
        skel_mesh != nullptr && skel_mesh->skeletal_model != nullptr) {
        if (const auto* skeleton = skel_mesh->skeletal_model->skeleton()) {
            create_bone_bodies(node, *skeleton);
        }
    }

    const auto* col_comp = node.components.get<ColliderComponent>();
    if (!col_comp) return;

    // Default to static if no rigidbody descriptor
    static const auto default_static = [] {
        RigidbodyComponent rb;
        rb.body_type = BodyType::Static;
        return rb;
    }();
    const auto* rb_comp = node.components.get<RigidbodyComponent>();
    const auto& rb = rb_comp ? *rb_comp : default_static;
    const auto& col = *col_comp;

    // Create shape, scaling by object's scale
    JPH::ShapeRefC shape;
    glm::vec3 s = spatial->transform.scale;
    bool mesh_supports_dynamic = false;

    // Collision authored on the mesh wins over the component's own fields,
    // so a placed object needs no per-instance setup.
    auto effective_shape = effective_collider_shape(col, node);

    // The collider's surface rides on the shape, so a ray or a foot that
    // lands on this body reads it back.
    const JPH::PhysicsMaterial* material = impl_->material_for(col.surface);

    switch (effective_shape) {
        case ColliderShape::Box: {
            auto he = clamp_box_half_extents(col.half_extents * s);
            shape = new JPH::BoxShape(JPH::Vec3(he.x, he.y, he.z), JPH::cDefaultConvexRadius,
                                      material);
            break;
        }
        case ColliderShape::Sphere: {
            float r = clamp_positive(col.radius * std::max({s.x, s.y, s.z}));
            shape = new JPH::SphereShape(r, material);
            break;
        }
        case ColliderShape::Capsule: {
            float r = clamp_positive(col.radius * std::max(s.x, s.z));
            float half_h = clamp_positive(col.height * 0.5f * s.y);
            shape = new JPH::CapsuleShape(half_h, r, material);
            break;
        }
        case ColliderShape::Mesh: {
            auto* mesh = node.components.get<MeshComponent>();
            if (!mesh || !mesh->model) {
                FJELL_CORE_WARN("Object '{}' has Mesh collider but no mesh", node.name);
                return;
            }
            auto collision = collision_mesh_of(*mesh->model);
            const auto& primitives = mesh->model->collision_primitives();
            if (primitives.empty() || !collision.valid()) {
                FJELL_CORE_WARN("Object '{}' has Mesh collider but no collision mesh data",
                                node.name);
                return;
            }

            shape = build_authored_shape(primitives, collision, s, node.name, material,
                                         mesh_supports_dynamic);
            if (!shape) return;

            // Triangle meshes can't back a moving body in Jolt.
            if (!mesh_supports_dynamic && rb.body_type != BodyType::Static) {
                FJELL_CORE_WARN("Object '{}' has a triangle-mesh collider on a non-static "
                                "body — forcing static. Use a convex hull to keep it moving.",
                                node.name);
            }
            break;
        }
    }

    if (!shape) return;

    // Apply offset if non-zero
    if (glm::length(col.offset) > 0.001f) {
        shape = new JPH::OffsetCenterOfMassShape(shape, to_jolt(col.offset));
    }

    // Determine motion type. Only triangle-mesh colliders are static-only;
    // a hull moves like any other convex shape.
    auto effective_type = rb.body_type;
    if (effective_shape == ColliderShape::Mesh && effective_type != BodyType::Static &&
        !mesh_supports_dynamic) {
        effective_type = BodyType::Static;
    }

    // A collider on the Trigger channel is a sensor by definition, so the
    // is_trigger flag and the channel mean the same thing and either sets
    // it. The flag predates channels and still works on its own.
    bool is_sensor = col.is_trigger ||
                     col.filter.channel == CollisionChannel::trigger;

    JPH::EMotionType motion;
    if (is_sensor) {
        // Sensors are kinematic so they can be moved and generate contacts
        motion = (effective_type == BodyType::Static)
            ? JPH::EMotionType::Static : JPH::EMotionType::Kinematic;
    } else {
        switch (effective_type) {
            case BodyType::Static:
                motion = JPH::EMotionType::Static;
                break;
            case BodyType::Kinematic:
                motion = JPH::EMotionType::Kinematic;
                break;
            case BodyType::Dynamic:
            default:
                motion = JPH::EMotionType::Dynamic;
                break;
        }
    }

    // The filter decides what this body collides with; the layer is just
    // the handle Jolt uses to ask about it.
    auto filter = col.filter;
    if (is_sensor) filter.channel = CollisionChannel::trigger;
    JPH::ObjectLayer layer = impl_->filter_table.layer_for(
        filter, motion != JPH::EMotionType::Static);

    JPH::BodyCreationSettings body_settings(
        shape,
        to_jolt(spatial->transform.position),
        to_jolt_quat(spatial->transform.rotation),
        motion, layer);

    body_settings.mFriction = rb.friction;
    body_settings.mRestitution = rb.restitution;
    body_settings.mLinearDamping = rb.linear_damping;
    body_settings.mAngularDamping = rb.angular_damping;
    body_settings.mGravityFactor = rb.gravity_enabled ? 1.0f : 0.0f;

    // Native sensor support — Jolt handles contact callbacks without physical response
    if (is_sensor) {
        body_settings.mIsSensor = true;
    }

    // Enable kinematic contact generation against static and other kinematic bodies
    if (effective_type == BodyType::Kinematic) {
        body_settings.mCollideKinematicVsNonDynamic = true;
    }

    if (effective_type == BodyType::Dynamic) {
        float mass = rb.mass > 0.0f ? rb.mass : 1.0f;

        // A body that floats says how heavy it is twice — once as a mass and
        // once as a density against water — and the two can disagree. They
        // are the same property, so the density wins and the mass follows
        // from it: that is the number an artist reasons about when they want
        // something to float, and a cube set to a tenth of water's density
        // that still weighed a tonne would sink while its dial promised
        // otherwise.
        if (const auto* buoyancy = node.components.get<BuoyancyComponent>()) {
            const BuoyancySamples samples =
                buoyancy_samples(*buoyancy, node.components.get<ColliderComponent>());
            if (samples.volume > 0.0f && buoyancy->density > 0.0f) {
                mass = samples.volume * buoyancy->density * WATER_DENSITY;
            }
        }

        body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        body_settings.mMassPropertiesOverride.mMass = std::max(mass, 1e-3f);
    }

    auto body_id = bi.CreateAndAddBody(body_settings,
        effective_type == BodyType::Static ? JPH::EActivation::DontActivate
                                          : JPH::EActivation::Activate);

    if (body_id.IsInvalid()) {
        FJELL_CORE_ERROR("Failed to create physics body for '{}'", node.name);
        return;
    }

    impl_->object_to_body[node.id] = body_id;
    impl_->body_to_object[body_id.GetIndexAndSequenceNumber()] = node.id;
    impl_->body_scales[node.id] = spatial->transform.scale;
}

void PhysicsWorld::create_bodies(Scene& scene) {
    for (auto& obj : scene.objects()) {
        create_body(scene, *obj);
    }

    FJELL_CORE_INFO("Physics: created {} bodies, {} characters",
                     impl_->object_to_body.size(), impl_->characters.size());
}

// ── Step ────────────────────────────────────────────────────────────────

// Brings the character in line with its component after an authored value
// changed. A new capsule is refused when it would push into geometry — the
// crouched character under a table cannot stand up — and the component is
// then set back to the capsule it actually has, so gameplay reading it sees
// the truth rather than its own request.
void PhysicsWorld::apply_character_changes(CharacterEntry& entry, CharacterComponent& cc,
                                           const Node3D& node) {
    auto& built = entry.built;
    auto& character = *entry.character;
    constexpr float EPS = 1e-5f;
    auto same = [](float a, float b) { return std::abs(a - b) <= EPS; };
    auto same3 = [&](const glm::vec3& a, const glm::vec3& b) {
        return glm::all(glm::epsilonEqual(a, b, EPS));
    };

    auto wanted = scaled_capsule(cc, node.transform.scale);
    wanted.radius = clamp_positive(wanted.radius);
    wanted.height = clamp_positive(wanted.height);

    bool size_changed = !same(wanted.radius, built.capsule.radius) ||
                        !same(wanted.height, built.capsule.height);
    bool center_changed = !same3(wanted.center, built.capsule.center);
    if (size_changed || center_changed) {
        // The center moves first so the feet stay where they are: a taller
        // capsule tested at the crouched center would dip into the floor and
        // be refused for the wrong reason.
        character.SetShapeOffset(to_jolt(wanted.center));
        if (size_changed) {
            JPH::RefConst<JPH::Shape> shape =
                new JPH::CapsuleShape(wanted.height * 0.5f, wanted.radius);
            float max_penetration =
                1.5f * impl_->physics_system->GetPhysicsSettings().mPenetrationSlop;
            bool fits = character.SetShape(
                shape, max_penetration,
                impl_->physics_system->GetDefaultBroadPhaseLayerFilter(entry.layer),
                impl_->physics_system->GetDefaultLayerFilter(entry.layer),
                {}, {}, *impl_->temp_allocator);
            if (fits) {
                character.SetInnerBodyShape(shape);
                built.capsule.radius = wanted.radius;
                built.capsule.height = wanted.height;
            } else {
                // Undo the scale so the authored fields read back in their
                // own units, the same way they were written.
                float sxz = std::max(node.transform.scale.x, node.transform.scale.z);
                cc.capsule_radius = sxz > 0.0f ? built.capsule.radius / sxz : built.capsule.radius;
                cc.capsule_height = node.transform.scale.y > 0.0f
                    ? built.capsule.height / node.transform.scale.y : built.capsule.height;
                wanted = scaled_capsule(cc, node.transform.scale);
                character.SetShapeOffset(to_jolt(wanted.center));
            }
        }
        character.set_supporting_volume(supporting_volume_for(wanted));
        built.capsule.center = wanted.center;
    }
    built.scale = node.transform.scale;

    if (!same(cc.max_slope_angle, built.max_slope_angle)) {
        character.SetMaxSlopeAngle(JPH::DegreesToRadians(cc.max_slope_angle));
        built.max_slope_angle = cc.max_slope_angle;
    }
    if (!same(cc.mass, built.mass)) {
        float mass = std::max(cc.mass, 1.0f);
        character.SetMass(mass);
        character.SetMaxStrength(mass * impl_->physics_system->GetGravity().Length());
        built.mass = cc.mass;
    }
    if (cc.filter.channel != built.filter.channel ||
        cc.filter.block_mask != built.filter.block_mask ||
        cc.filter.overlap_mask != built.filter.overlap_mask) {
        entry.layer = impl_->filter_table.layer_for(cc.filter, true);
        auto inner_id = character.GetInnerBodyID();
        if (!inner_id.IsInvalid()) {
            impl_->physics_system->GetBodyInterface().SetObjectLayer(inner_id, entry.layer);
        }
        built.filter = cc.filter;
    }
}

void PhysicsWorld::step(float dt, Scene& scene) {
    FJELL_PROFILE_SCOPE_N("physics_step");
    if (dt <= 0.0f) return;
    impl_->contact_listener.clear();
    auto& bi = impl_->physics_system->GetBodyInterface();

    // A shape cannot be rescaled after creation, so a node whose scale changed
    // needs its body rebuilt. Checked against the scale it was built at rather
    // than a dirty flag, which would have to be set by everything that can
    // move a transform — the inspector, scripts, animation, the gizmo.
    for (auto& obj : scene.objects()) {
        auto* spatial = obj->as_spatial();
        if (spatial == nullptr) continue;

        auto it = impl_->body_scales.find(obj->id);
        if (it == impl_->body_scales.end()) continue;
        if (glm::all(glm::epsilonEqual(it->second, spatial->transform.scale, 1e-5F))) {
            continue;
        }

        // Rebuilding drops the body's velocity with it, so it is carried over —
        // a rescaled object should keep moving, not stop dead.
        auto velocity = get_linear_velocity(obj->id);
        auto angular = get_angular_velocity(obj->id);
        create_body(scene, *obj);
        set_linear_velocity(obj->id, velocity);
        set_angular_velocity(obj->id, angular);
    }

    // Move bone bodies onto their bones. MoveKinematic rather than a teleport
    // so each body carries the velocity implied by the motion — a limb sweeping
    // through a dynamic object pushes it instead of passing through.
    if (!impl_->bone_bodies.empty()) {
        for (const auto& entry : impl_->bone_bodies) {
            auto* node = scene.find_spatial(entry.object_id);
            if (node == nullptr) continue;
            auto* anim = node->components.get<AnimatorComponent>();
            if (anim == nullptr) continue;

            const auto& bones = anim->bone_matrices();
            if (entry.bone < 0 || static_cast<size_t>(entry.bone) >= bones.size()) {
                continue;
            }

            // Bone transforms are model-space, so the node's world matrix puts
            // them where the character actually is.
            glm::mat4 world = node->world_matrix_ * bones[static_cast<size_t>(entry.bone)];
            glm::vec3 position{world[3]};
            // The shape already baked the node's scale, so the rotation has to
            // come from a normalized basis or it would be scaled twice.
            glm::mat3 basis{glm::normalize(glm::vec3(world[0])),
                            glm::normalize(glm::vec3(world[1])),
                            glm::normalize(glm::vec3(world[2]))};

            bi.MoveKinematic(entry.body, to_jolt(position),
                             to_jolt_quat(glm::quat_cast(basis)),
                             std::max(dt, 1.0f / 120.0f));
        }
    }

    // Sync kinematic bodies: push engine transforms -> Jolt
    for (auto& obj : scene.objects()) {
        const auto* rb = obj->components.get<RigidbodyComponent>();
        if (!rb || rb->body_type != BodyType::Kinematic) continue;
        auto* spatial = obj->as_spatial();
        if (!spatial) continue;

        auto it = impl_->object_to_body.find(obj->id);
        if (it == impl_->object_to_body.end()) continue;

        bi.MoveKinematic(it->second,
                         to_jolt(spatial->transform.position),
                         to_jolt_quat(spatial->transform.rotation),
                         std::max(dt, 1.0f / 120.0f));
    }

    // Update character controllers before physics step
    JPH::Vec3 gravity = impl_->physics_system->GetGravity();
    // Two characters touching each other are both aware of it, so a pair is
    // reported once per step no matter which side found it.
    std::unordered_set<uint64_t> reported_pairs;
    // `surface` is what the character touched; the character itself has none,
    // so the other side's event carries 0.
    auto report_character_contact = [&](bool enter, bool sensor, uint32_t self, uint32_t other,
                                        const glm::vec3& point, const glm::vec3& normal,
                                        uint32_t surface) {
        uint64_t pair = (static_cast<uint64_t>(std::min(self, other)) << 32) |
                        std::max(self, other);
        if (!reported_pairs.insert(pair).second) return;
        auto& listener = impl_->contact_listener;
        auto& events = sensor ? (enter ? listener.trigger_enter_events : listener.trigger_exit_events)
                              : (enter ? listener.collision_enter_events : listener.collision_exit_events);
        // Normals point from the receiving node toward the other, as the body
        // listener reports them.
        events.push_back({self, {other, point, normal, surface}});
        events.push_back({other, {self, point, -normal, 0}});
    };
    for (auto& entry : impl_->characters) {
        auto* node = scene.find_spatial(entry.object_id);
        if (!node) continue;
        auto* cc = node->components.get<CharacterComponent>();
        if (!cc) continue;

        // The node is the authority on where the character is between steps.
        // Rotation always follows it; position only when it was moved from
        // outside, so the solver's own sub-step placement is not overwritten
        // by the rounded copy written back last step.
        glm::vec3 world_position;
        glm::quat world_rotation;
        world_placement(scene, *node, world_position, world_rotation);
        entry.character->SetRotation(to_jolt_quat(world_rotation));
        if (glm::distance(world_position, entry.synced_position) > 1e-4f) {
            entry.character->SetPosition(to_jolt(world_position));
            entry.synced_position = world_position;
        }

        apply_character_changes(entry, *cc, *node);

        if (!cc->simulate) {
            // Nothing is swept, so nothing is touched; whatever it was
            // touching has ended as far as gameplay is concerned.
            for (const auto& old : entry.contacts) {
                report_character_contact(false, old.sensor, entry.object_id, old.other_id,
                                         world_position, glm::vec3{0.0f}, 0);
            }
            entry.contacts.clear();
            continue;
        }

        glm::vec3 vel = cc->velocity;
        if (cc->ground_state == CharacterGroundState::OnGround) {
            // Standing on something: vertical velocity is at least whatever the
            // ground is doing, so a landed character stops pushing into the
            // floor and a moving platform carries it. Anything faster upward
            // was written by gameplay and is a jump, so it stays.
            entry.character->UpdateGroundVelocity();
            vel.y = std::max(vel.y, to_glm(entry.character->GetGroundVelocity()).y);
        } else {
            // Airborne, or on ground too steep to stand on: gravity applies,
            // which is what makes a steep slope something to slide down.
            vel += to_glm(gravity) * dt;
        }
        cc->velocity = vel;

        entry.character->SetLinearVelocity(to_jolt(vel));

        JPH::CharacterVirtual::ExtendedUpdateSettings update_settings;
        update_settings.mWalkStairsStepUp = JPH::Vec3(0.0f, std::max(cc->step_up, 0.0f), 0.0f);
        update_settings.mStickToFloorStepDown = JPH::Vec3(0.0f, -std::max(cc->step_down, 0.0f), 0.0f);
        entry.character->ExtendedUpdate(dt, gravity, update_settings,
            impl_->physics_system->GetDefaultBroadPhaseLayerFilter(entry.layer),
            impl_->physics_system->GetDefaultLayerFilter(entry.layer),
            {},
            {},
            *impl_->temp_allocator);

        // What the sweep touched this step. A solid contact is one the solver
        // pushed against; a sensor never pushes, so it counts once the shape
        // is inside it rather than merely approaching. Several contacts with
        // one body — a floor's triangles — are one contact with that node.
        std::vector<CharacterEntry::Contact> touched;
        for (const auto& contact : entry.character->GetActiveContacts()) {
            if (contact.mBodyB.IsInvalid()) continue;
            bool touching = contact.mIsSensorB ? contact.mDistance < 1e-3f
                                               : contact.mHadCollision;
            if (!touching) continue;
            auto other = impl_->body_to_object.find(contact.mBodyB.GetIndexAndSequenceNumber());
            if (other == impl_->body_to_object.end()) continue;
            if (other->second == entry.object_id) continue;

            bool known = std::any_of(touched.begin(), touched.end(), [&](const auto& t) {
                return t.other_id == other->second;
            });
            if (known) continue;
            touched.push_back({other->second, contact.mIsSensorB});

            bool was_touching = std::any_of(entry.contacts.begin(), entry.contacts.end(),
                                            [&](const auto& t) { return t.other_id == other->second; });
            if (!was_touching) {
                report_character_contact(true, contact.mIsSensorB, entry.object_id, other->second,
                                         to_glm(JPH::Vec3(contact.mPosition)),
                                         -to_glm(contact.mContactNormal),
                                         impl_->surface_of(contact.mMaterial));
            }
        }
        for (const auto& old : entry.contacts) {
            bool still = std::any_of(touched.begin(), touched.end(),
                                     [&](const auto& t) { return t.other_id == old.other_id; });
            if (!still) {
                report_character_contact(false, old.sensor, entry.object_id, old.other_id,
                                         to_glm(entry.character->GetPosition()), glm::vec3{0.0f},
                                         0);
            }
        }
        entry.contacts = std::move(touched);
    }

    // Step physics
    constexpr int collision_steps = 1;
    impl_->physics_system->Update(dt, collision_steps,
                                   impl_->temp_allocator.get(),
                                   impl_->job_system.get());

    // Sync dynamic bodies: pull Jolt transforms -> engine
    for (auto& obj : scene.objects()) {
        const auto* rb = obj->components.get<RigidbodyComponent>();
        if (!rb || rb->body_type != BodyType::Dynamic) continue;
        auto* spatial = obj->as_spatial();
        if (!spatial) continue;

        auto it = impl_->object_to_body.find(obj->id);
        if (it == impl_->object_to_body.end()) continue;

        auto pos = bi.GetPosition(it->second);
        auto rot = bi.GetRotation(it->second);

        spatial->transform.position = to_glm(pos);
        spatial->transform.rotation = to_glm_quat(rot);
    }

    // Sync character controllers: pull Jolt state -> engine
    for (auto& entry : impl_->characters) {
        auto* node = scene.find_spatial(entry.object_id);
        if (!node) continue;
        auto* cc = node->components.get<CharacterComponent>();
        if (!cc) continue;
        if (!cc->simulate) continue;

        glm::vec3 world_position = to_glm(entry.character->GetPosition());
        scene.set_world_position(*node, world_position);
        entry.synced_position = world_position;

        cc->velocity = to_glm(entry.character->GetLinearVelocity());
        cc->ground_velocity = to_glm(entry.character->GetGroundVelocity());
        cc->ground_normal = to_glm(entry.character->GetGroundNormal());
        cc->ground_surface = impl_->surface_of(entry.character->GetGroundMaterial());

        switch (entry.character->GetGroundState()) {
            case JPH::CharacterVirtual::EGroundState::OnGround:
                cc->ground_state = CharacterGroundState::OnGround;
                break;
            case JPH::CharacterVirtual::EGroundState::OnSteepGround:
                cc->ground_state = CharacterGroundState::OnSteepGround;
                break;
            case JPH::CharacterVirtual::EGroundState::NotSupported:
                cc->ground_state = CharacterGroundState::NotSupported;
                break;
            case JPH::CharacterVirtual::EGroundState::InAir:
            default:
                cc->ground_state = CharacterGroundState::InAir;
                break;
        }
    }
}

// ── Body removal ────────────────────────────────────────────────────────

void PhysicsWorld::destroy_body(uint32_t object_id) {
    // A node can own several of these at once — a character with a movement
    // capsule and per-bone hit shapes owns both kinds — so each is cleaned up
    // in turn rather than the first match winning.

    // Bone bodies first: a character owning them would otherwise leak, since
    // its own teardown below returns early.
    if (!impl_->bone_bodies.empty()) {
        auto& bi_bone = impl_->physics_system->GetBodyInterface();
        std::erase_if(impl_->bone_bodies, [&](const Impl::BoneBody& entry) {
            if (entry.object_id != object_id) return false;
            impl_->body_to_object.erase(entry.body.GetIndexAndSequenceNumber());
            bi_bone.RemoveBody(entry.body);
            bi_bone.DestroyBody(entry.body);
            return true;
        });
    }

    // A character owns an inner body registered under the same node, so it has
    // to go through its own teardown rather than the body interface.
    auto character = std::find_if(impl_->characters.begin(), impl_->characters.end(),
                                  [object_id](const CharacterEntry& entry) {
                                      return entry.object_id == object_id;
                                  });
    if (character != impl_->characters.end()) {
        auto inner_id = character->character->GetInnerBodyID();
        if (!inner_id.IsInvalid()) {
            impl_->body_to_object.erase(inner_id.GetIndexAndSequenceNumber());
            impl_->contact_listener.character_bodies.erase(inner_id.GetIndexAndSequenceNumber());
        }
        impl_->characters.erase(character);
        return;
    }

    auto it = impl_->object_to_body.find(object_id);
    if (it == impl_->object_to_body.end()) return;

    auto& bi = impl_->physics_system->GetBodyInterface();
    bi.RemoveBody(it->second);
    bi.DestroyBody(it->second);

    impl_->body_to_object.erase(it->second.GetIndexAndSequenceNumber());
    impl_->object_to_body.erase(it);
    impl_->body_scales.erase(object_id);
}

// ── Forces / Velocities ─────────────────────────────────────────────────

void PhysicsWorld::apply_force(uint32_t object_id, const glm::vec3& force) {
    auto it = impl_->object_to_body.find(object_id);
    if (it == impl_->object_to_body.end()) return;
    impl_->physics_system->GetBodyInterface().AddForce(it->second, to_jolt(force));
}

void PhysicsWorld::apply_force_at(uint32_t object_id, const glm::vec3& force,
                                  const glm::vec3& point) {
    auto it = impl_->object_to_body.find(object_id);
    if (it == impl_->object_to_body.end()) return;
    impl_->physics_system->GetBodyInterface().AddForce(it->second, to_jolt(force),
                                                       to_jolt(point));
}

void PhysicsWorld::apply_impulse(uint32_t object_id, const glm::vec3& impulse) {
    auto it = impl_->object_to_body.find(object_id);
    if (it == impl_->object_to_body.end()) return;
    impl_->physics_system->GetBodyInterface().AddImpulse(it->second, to_jolt(impulse));
}

void PhysicsWorld::set_linear_velocity(uint32_t object_id, const glm::vec3& vel) {
    auto it = impl_->object_to_body.find(object_id);
    if (it == impl_->object_to_body.end()) return;
    impl_->physics_system->GetBodyInterface().SetLinearVelocity(it->second, to_jolt(vel));
}

glm::vec3 PhysicsWorld::get_linear_velocity(uint32_t object_id) const {
    auto it = impl_->object_to_body.find(object_id);
    if (it == impl_->object_to_body.end()) return glm::vec3(0.0f);
    return to_glm(impl_->physics_system->GetBodyInterface().GetLinearVelocity(it->second));
}

void PhysicsWorld::set_angular_velocity(uint32_t object_id, const glm::vec3& vel) {
    auto it = impl_->object_to_body.find(object_id);
    if (it == impl_->object_to_body.end()) return;
    impl_->physics_system->GetBodyInterface().SetAngularVelocity(it->second, to_jolt(vel));
}

glm::vec3 PhysicsWorld::get_angular_velocity(uint32_t object_id) const {
    auto it = impl_->object_to_body.find(object_id);
    if (it == impl_->object_to_body.end()) return glm::vec3(0.0f);
    return to_glm(impl_->physics_system->GetBodyInterface().GetAngularVelocity(it->second));
}

// ── Raycast ─────────────────────────────────────────────────────────────

// Translates an engine-side QueryFilter into the body filter Jolt asks with.
// The channel test goes through the same filter table the pair filter uses, so
// a query and a contact agree about what a channel means.
class QueryBodyFilter final : public JPH::BodyFilter {
public:
    const QueryFilter* query{nullptr};
    const FilterTable* table{nullptr};
    const std::unordered_map<uint32_t, uint32_t>* body_to_object{nullptr};

    [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override {
        if (query == nullptr) return true;

        if (const auto* filter = table != nullptr ? table->find(body.GetObjectLayer())
                                                  : nullptr) {
            if (!query->allows(filter->channel)) return false;
        }

        if (!query->ignore_nodes.empty() && body_to_object != nullptr) {
            auto it = body_to_object->find(body.GetID().GetIndexAndSequenceNumber());
            if (it != body_to_object->end() && query->ignores(it->second)) return false;
        }
        return true;
    }
};

// Fills a RaycastHit from a body and a point on it. Surface normals need the
// body locked, so this is the one place that does it.
static void fill_hit(const PhysicsWorld::Impl& impl, JPH::BodyID body_id,
                     JPH::SubShapeID sub_shape, JPH::RVec3Arg point, float distance,
                     RaycastHit& out) {
    out.position = to_glm(point);
    out.distance = distance;

    auto it = impl.body_to_object.find(body_id.GetIndexAndSequenceNumber());
    out.object_id = it != impl.body_to_object.end() ? it->second : 0;

    JPH::BodyLockRead lock(impl.physics_system->GetBodyLockInterface(), body_id);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        out.normal = to_glm(body.GetWorldSpaceSurfaceNormal(sub_shape, point));
        out.surface = impl.surface_of(body.GetShape()->GetMaterial(sub_shape));
    }
}

// The shape a cast or overlap sweeps, in the engine's vocabulary.
static JPH::ShapeRefC query_shape(QueryShape shape, const glm::vec3& extents) {
    switch (shape) {
        case QueryShape::sphere:
            return new JPH::SphereShape(clamp_positive(extents.x));
        case QueryShape::box: {
            auto he = clamp_box_half_extents(extents);
            return new JPH::BoxShape(JPH::Vec3(he.x, he.y, he.z));
        }
        case QueryShape::capsule:
            return new JPH::CapsuleShape(clamp_positive(extents.y * 0.5f),
                                         clamp_positive(extents.x));
    }
    return {};
}

void PhysicsWorld::update_terrain_heights(uint32_t object_id, uint32_t x, uint32_t z,
                                          uint32_t size_x, uint32_t size_z,
                                          const float* heights) {
    if (heights == nullptr || size_x == 0 || size_z == 0) return;

    auto it = impl_->object_to_body.find(object_id);
    if (it == impl_->object_to_body.end()) return;

    JPH::BodyLockWrite lock(impl_->physics_system->GetBodyLockInterface(), it->second);
    if (!lock.Succeeded()) return;

    auto* shape = const_cast<JPH::Shape*>(lock.GetBody().GetShape());
    if (shape == nullptr || shape->GetSubType() != JPH::EShapeSubType::HeightField) {
        return;
    }
    auto* field = static_cast<JPH::HeightFieldShape*>(shape);

    // SetHeights needs both origin and size block-aligned, so grow the rect
    // outward to the enclosing blocks rather than rejecting an unaligned brush
    // footprint. GetSampleCount is itself rounded up to a block multiple, so
    // clamping the far edge to it keeps the size aligned too.
    const uint32_t block = field->GetBlockSize();
    const uint32_t samples = field->GetSampleCount();
    if (block == 0 || x >= samples || z >= samples) return;

    const uint32_t x0 = (x / block) * block;
    const uint32_t z0 = (z / block) * block;
    const uint32_t x1 = std::min(((x + size_x + block - 1) / block) * block, samples);
    const uint32_t z1 = std::min(((z + size_z + block - 1) / block) * block, samples);
    if (x1 <= x0 || z1 <= z0) return;

    const uint32_t w = x1 - x0;
    const uint32_t h = z1 - z0;

    // The caller's rect covers only part of the aligned region, so seed the
    // buffer with what the shape already holds and overwrite the middle.
    std::vector<float> block_heights(static_cast<size_t>(w) * h);
    field->GetHeights(x0, z0, w, h, block_heights.data(), static_cast<intptr_t>(w));

    for (uint32_t row = 0; row < size_z; ++row) {
        const uint32_t dst_z = z + row;
        if (dst_z < z0 || dst_z >= z1) continue;
        for (uint32_t col = 0; col < size_x; ++col) {
            const uint32_t dst_x = x + col;
            if (dst_x < x0 || dst_x >= x1) continue;
            block_heights[(static_cast<size_t>(dst_z - z0) * w) + (dst_x - x0)] =
                heights[(static_cast<size_t>(row) * size_x) + col];
        }
    }

    field->SetHeights(x0, z0, w, h, block_heights.data(), static_cast<intptr_t>(w),
                      *impl_->temp_allocator);
}

void PhysicsWorld::update_terrain_surfaces(uint32_t object_id, const TerrainAsset& terrain,
                                           uint32_t x, uint32_t z, uint32_t size_x,
                                           uint32_t size_z) {
    if (size_x == 0 || size_z == 0) return;

    auto it = impl_->object_to_body.find(object_id);
    if (it == impl_->object_to_body.end()) return;

    JPH::BodyLockWrite lock(impl_->physics_system->GetBodyLockInterface(), it->second);
    if (!lock.Succeeded()) return;

    auto* shape = const_cast<JPH::Shape*>(lock.GetBody().GetShape());
    if (shape == nullptr || shape->GetSubType() != JPH::EShapeSubType::HeightField) {
        return;
    }
    auto* field = static_cast<JPH::HeightFieldShape*>(shape);
    // A field built without materials has nothing to update; it was built
    // that way because no layer had a surface, and layers are not added
    // during play.
    if (field->GetMaterialList().empty()) return;

    // Weight texels to the quads they cover: the field has one quad fewer
    // than samples per side, the weight grid its own count.
    const uint32_t quads = terrain.sample_count() > 1 ? terrain.sample_count() - 1 : 0;
    const uint32_t side = terrain.weight_count();
    if (quads == 0 || side == 0) return;
    const uint32_t qx0 = std::min((x * quads) / side, quads - 1);
    const uint32_t qz0 = std::min((z * quads) / side, quads - 1);
    const uint32_t qx1 = std::min((((x + size_x) * quads) + side - 1) / side, quads);
    const uint32_t qz1 = std::min((((z + size_z) * quads) + side - 1) / side, quads);
    if (qx1 <= qx0 || qz1 <= qz0) return;
    const uint32_t w = qx1 - qx0;
    const uint32_t h = qz1 - qz0;

    std::vector<uint8_t> indices(static_cast<size_t>(w) * h);
    terrain.dominant_layers(qx0, qz0, w, h, indices.data());
    field->SetMaterials(qx0, qz0, w, h, indices.data(), static_cast<intptr_t>(w), nullptr,
                        *impl_->temp_allocator);
}

bool PhysicsWorld::ground_height_at(const glm::vec3& world_position, float max_distance,
                                    float& out_height) const {
    RaycastHit hit;
    if (!raycast(world_position, glm::vec3{0.0f, -1.0f, 0.0f}, max_distance, hit)) {
        return false;
    }
    out_height = hit.position.y;
    return true;
}

bool PhysicsWorld::ground_normal_at(const glm::vec3& world_position, float max_distance,
                                    glm::vec3& out_normal) const {
    RaycastHit hit;
    if (!raycast(world_position, glm::vec3{0.0f, -1.0f, 0.0f}, max_distance, hit)) {
        return false;
    }
    out_normal = hit.normal;
    return true;
}

bool PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction,
                           float max_distance, RaycastHit& out_hit,
                           const QueryFilter& filter) const {
    auto dir_norm = glm::normalize(direction);
    JPH::RRayCast ray(to_jolt(origin), to_jolt(dir_norm * max_distance));
    JPH::RayCastResult result;

    QueryBodyFilter body_filter;
    body_filter.query = &filter;
    body_filter.table = &impl_->filter_table;
    body_filter.body_to_object = &impl_->body_to_object;

    if (!impl_->physics_system->GetNarrowPhaseQuery().CastRay(
            ray, result, {}, {}, body_filter)) {
        return false;
    }

    fill_hit(*impl_, result.mBodyID,
             result.mSubShapeID2, ray.GetPointOnRay(result.mFraction),
             max_distance * result.mFraction, out_hit);
    return true;
}

std::vector<RaycastHit> PhysicsWorld::raycast_all(const glm::vec3& origin,
                                                  const glm::vec3& direction,
                                                  float max_distance,
                                                  const QueryFilter& filter) const {
    auto dir_norm = glm::normalize(direction);
    JPH::RRayCast ray(to_jolt(origin), to_jolt(dir_norm * max_distance));

    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    QueryBodyFilter body_filter;
    body_filter.query = &filter;
    body_filter.table = &impl_->filter_table;
    body_filter.body_to_object = &impl_->body_to_object;

    impl_->physics_system->GetNarrowPhaseQuery().CastRay(
        ray, {}, collector, {}, {}, body_filter);
    collector.Sort();

    std::vector<RaycastHit> hits;
    hits.reserve(collector.mHits.size());
    for (const auto& hit : collector.mHits) {
        RaycastHit out;
        fill_hit(*impl_, hit.mBodyID,
                 hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction),
                 max_distance * hit.mFraction, out);
        hits.push_back(out);
    }
    return hits;
}

bool PhysicsWorld::shape_cast(QueryShape shape, const glm::vec3& extents,
                              const glm::vec3& origin, const glm::vec3& direction,
                              float max_distance, RaycastHit& out_hit,
                              const QueryFilter& filter) const {
    auto jolt_shape = query_shape(shape, extents);
    if (!jolt_shape) return false;

    auto dir_norm = glm::normalize(direction);
    JPH::RShapeCast cast(jolt_shape, JPH::Vec3::sReplicate(1.0f),
                         JPH::RMat44::sTranslation(to_jolt(origin)),
                         to_jolt(dir_norm * max_distance));

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    QueryBodyFilter body_filter;
    body_filter.query = &filter;
    body_filter.table = &impl_->filter_table;
    body_filter.body_to_object = &impl_->body_to_object;

    JPH::ShapeCastSettings settings;
    impl_->physics_system->GetNarrowPhaseQuery().CastShape(
        cast, settings, JPH::RVec3::sZero(), collector, {}, {}, body_filter);

    if (!collector.HadHit()) return false;

    const auto& hit = collector.mHit;
    fill_hit(*impl_, hit.mBodyID2,
             hit.mSubShapeID2, cast.GetPointOnRay(hit.mFraction),
             max_distance * hit.mFraction, out_hit);
    // A swept shape's contact normal is the separating axis, which is more
    // useful here than the surface normal at the touch point.
    out_hit.normal = to_glm(-hit.mPenetrationAxis.Normalized());
    return true;
}

std::vector<uint32_t> PhysicsWorld::overlap(QueryShape shape, const glm::vec3& extents,
                                            const glm::vec3& position,
                                            const QueryFilter& filter) const {
    auto jolt_shape = query_shape(shape, extents);
    if (!jolt_shape) return {};

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    QueryBodyFilter body_filter;
    body_filter.query = &filter;
    body_filter.table = &impl_->filter_table;
    body_filter.body_to_object = &impl_->body_to_object;

    JPH::CollideShapeSettings settings;
    impl_->physics_system->GetNarrowPhaseQuery().CollideShape(
        jolt_shape, JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sTranslation(to_jolt(position)),
        settings, JPH::RVec3::sZero(), collector, {}, {}, body_filter);

    // One body can report several contacts; the caller wants nodes, once each.
    std::vector<uint32_t> nodes;
    for (const auto& hit : collector.mHits) {
        auto it = impl_->body_to_object.find(hit.mBodyID2.GetIndexAndSequenceNumber());
        if (it == impl_->body_to_object.end()) continue;
        if (std::find(nodes.begin(), nodes.end(), it->second) == nodes.end()) {
            nodes.push_back(it->second);
        }
    }
    return nodes;
}

void PhysicsWorld::set_gravity(const glm::vec3& gravity) {
    impl_->physics_system->SetGravity(to_jolt(gravity));
}

glm::vec3 PhysicsWorld::gravity() const {
    return to_glm(impl_->physics_system->GetGravity());
}

// ── Collision/trigger event access ──────────────────────────────────────

const std::vector<std::pair<uint32_t, CollisionEvent>>& PhysicsWorld::collision_enter_events() const {
    return impl_->contact_listener.collision_enter_events;
}

const std::vector<std::pair<uint32_t, CollisionEvent>>& PhysicsWorld::collision_exit_events() const {
    return impl_->contact_listener.collision_exit_events;
}

const std::vector<std::pair<uint32_t, CollisionEvent>>& PhysicsWorld::trigger_enter_events() const {
    return impl_->contact_listener.trigger_enter_events;
}

const std::vector<std::pair<uint32_t, CollisionEvent>>& PhysicsWorld::trigger_exit_events() const {
    return impl_->contact_listener.trigger_exit_events;
}

// ── Hull faces ──────────────────────────────────────────────────────────

// Declared in collision_shape.hpp, defined here because this is the file that
// is allowed to know which backend builds the hull. Callers get engine types
// and never learn where the faces came from.
std::vector<glm::vec3> convex_hull_edges(const std::vector<glm::vec3>& points) {
    // Four non-coplanar points are the minimum for a volume; Create() reports
    // the coplanar case as an error rather than returning a degenerate hull.
    if (points.size() < 4) return {};

    ensure_jolt_init();

    JPH::Array<JPH::Vec3> jolt_points;
    jolt_points.reserve(points.size());
    for (const auto& p : points) jolt_points.push_back(to_jolt(p));

    auto result = JPH::ConvexHullShapeSettings(jolt_points).Create();
    if (result.HasError()) return {};

    const auto* hull = static_cast<const JPH::ConvexHullShape*>(result.Get().GetPtr());

    // Jolt reports points relative to the center of mass, so shifting by it
    // returns them to the space the caller passed in.
    auto com = to_glm(hull->GetCenterOfMass());

    std::vector<glm::vec3> edges;
    std::vector<JPH::uint> face_indices;
    std::unordered_set<uint64_t> seen;

    for (JPH::uint f = 0, face_count = hull->GetNumFaces(); f < face_count; ++f) {
        JPH::uint vertex_count = hull->GetNumVerticesInFace(f);
        if (vertex_count < 3) continue;

        face_indices.resize(vertex_count);
        hull->GetFaceVertices(f, vertex_count, face_indices.data());

        // Every edge borders two faces, so the second visit is a duplicate.
        for (JPH::uint i = 0; i < vertex_count; ++i) {
            JPH::uint a = face_indices[i];
            JPH::uint b = face_indices[(i + 1) % vertex_count];
            uint64_t key = a < b ? (static_cast<uint64_t>(a) << 32) | b
                                 : (static_cast<uint64_t>(b) << 32) | a;
            if (!seen.insert(key).second) continue;

            edges.push_back(to_glm(hull->GetPoint(a)) + com);
            edges.push_back(to_glm(hull->GetPoint(b)) + com);
        }
    }
    return edges;
}

} // namespace fjell
