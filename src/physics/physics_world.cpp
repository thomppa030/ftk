#include "physics/physics_world.hpp"
#include "physics/collision_channel.hpp"
#include "physics/collision_shape.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "renderer/components/mesh_component.hpp"
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
    // (terrain, character inner bodies), so the broadphase mapping below stays
    // valid for them.
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
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        if (layer == broad_phase::STATIC) return "STATIC";
        if (layer == broad_phase::MOVING) return "MOVING";
        if (layer == broad_phase::SENSOR) return "SENSOR";
        return "UNKNOWN";
    }
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

        bool is_sensor = body1.IsSensor() || body2.IsSensor();

        // Cache this contact for exit tracking
        uint64_t pair_key = (static_cast<uint64_t>(key1) << 32) | key2;
        {
            std::lock_guard lock(contact_mutex_);
            active_contacts_[pair_key] = {obj1, obj2, is_sensor};
        }

        if (is_sensor) {
            std::lock_guard lock(event_mutex_);
            trigger_enter_events.push_back({obj1, {obj2, contact_point, contact_normal}});
            trigger_enter_events.push_back({obj2, {obj1, contact_point, -contact_normal}});
        } else {
            std::lock_guard lock(event_mutex_);
            collision_enter_events.push_back({obj1, {obj2, contact_point, contact_normal}});
            collision_enter_events.push_back({obj2, {obj1, contact_point, -contact_normal}});
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

        CollisionEvent exit_event{0, glm::vec3(0.0f), glm::vec3(0.0f)};

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

struct CharacterEntry {
    JPH::Ref<JPH::CharacterVirtual> character;
    uint32_t object_id;
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

    // A shape bakes the node's scale when it is created and cannot be
    // rescaled afterwards, so the scale each body was built at is kept to
    // notice when it no longer matches and the shape must be rebuilt.
    std::unordered_map<uint32_t, glm::vec3> body_scales;
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
                                            const std::string& owner) {
    switch (prim.kind) {
        case CollisionPrimitiveKind::box: {
            auto he = clamp_box_half_extents(prim.extents * scale);
            return new JPH::BoxShape(JPH::Vec3(he.x, he.y, he.z));
        }
        case CollisionPrimitiveKind::sphere: {
            float r = clamp_positive(prim.extents.x * std::max({scale.x, scale.y, scale.z}));
            return new JPH::SphereShape(r);
        }
        case CollisionPrimitiveKind::capsule: {
            float r = clamp_positive(prim.extents.x * std::max(scale.x, scale.z));
            float half_h = clamp_positive(prim.extents.y * 0.5f * scale.y);
            return new JPH::CapsuleShape(half_h, r);
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
            auto result = JPH::ConvexHullShapeSettings(points).Create();
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
            auto result = JPH::MeshShapeSettings(triangles).Create();
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
                                           bool& out_supports_dynamic) {
    out_supports_dynamic = true;
    for (const auto& prim : primitives) {
        if (!prim.is_convex()) out_supports_dynamic = false;
    }

    if (primitives.size() == 1) {
        const auto& prim = primitives.front();
        auto shape = build_primitive_shape(prim, geometry, scale, owner);
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
        auto shape = build_primitive_shape(prim, geometry, scale, owner);
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
                                       const TerrainAsset& terrain, const Node3D& node) {
    const auto& data = terrain.data();
    const uint32_t samples = data.sample_count();

    // Jolt subdivides the field into blocks and needs at least two per side.
    if (samples < 2 || data.heights.size() < static_cast<size_t>(samples) * samples) {
        return {};
    }

    // Sample spacing on X and Z; the node's scale stretches the whole field.
    const float spacing = data.header.world_size / static_cast<float>(samples - 1);
    const auto& s = node.transform.scale;

    JPH::HeightFieldShapeSettings settings(
        data.heights.data(),
        JPH::Vec3(0.0f, data.header.height_offset * s.y, 0.0f),
        JPH::Vec3(spacing * s.x, data.header.height_scale * s.y, spacing * s.z),
        samples);

    auto shape_result = settings.Create();
    if (shape_result.HasError()) {
        FJELL_CORE_ERROR("Failed to create terrain collision shape: {}",
                         shape_result.GetError().c_str());
        return {};
    }

    // The field grows from its origin corner rather than from its centre, so
    // the body sits at the node's position and the samples extend from there.
    // Terrain is world geometry, so it answers the channel table like any
    // other collider — a projectile set to ignore the world has to pass
    // through the ground too, not just through walls.
    auto layer = filters.layer_for(default_filter_for(CollisionChannel::world_static),
                                   false);

    JPH::BodyCreationSettings body_settings(
        shape_result.Get(),
        to_jolt(node.transform.position),
        to_jolt_quat(node.transform.rotation),
        JPH::EMotionType::Static, layer);

    return bi.CreateAndAddBody(body_settings, JPH::EActivation::DontActivate);
}

// ── Body creation ───────────────────────────────────────────────────────

// Creates the body for one node, if it should have one. Split out of the
// play-start loop so a node spawned mid-play gets the same treatment as one
// that was there from the beginning — the alternative is objects that look
// solid and fall through the world.
void PhysicsWorld::create_body(Node& node) {
    auto& bi = impl_->physics_system->GetBodyInterface();

    // A node that already has one is being rebuilt rather than added.
    destroy_body(node.id);

    auto* spatial = node.as_spatial();
    if (spatial == nullptr) return;

    // Character controller — handled in Step 5 section below
    auto* char_comp = node.components.get<CharacterComponent>();
    if (char_comp) {
        float r = clamp_positive(char_comp->capsule_radius);
        float half_h = clamp_positive(char_comp->capsule_height * 0.5f);

        JPH::CharacterVirtualSettings settings;
        settings.mShape = new JPH::CapsuleShape(half_h, r);
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(char_comp->max_slope_angle);
        settings.mMass = std::max(char_comp->mass, 1.0f);
        settings.mUp = JPH::Vec3(0, 1, 0);
        settings.mInnerBodyShape = new JPH::CapsuleShape(half_h, r);
        settings.mInnerBodyLayer = layers::MOVING;

        auto character = new JPH::CharacterVirtual(
            &settings,
            to_jolt(spatial->transform.position),
            to_jolt_quat(spatial->transform.rotation),
            0, // user data
            impl_->physics_system.get());

        impl_->characters.push_back({character, node.id});

        // Register inner body for contact resolution
        auto inner_id = character->GetInnerBodyID();
        if (!inner_id.IsInvalid()) {
            impl_->body_to_object[inner_id.GetIndexAndSequenceNumber()] = node.id;
        }
        return;
    }

    // Terrain is solid by virtue of being terrain — no collider component,
    // the way landscapes work in every engine that has them. The heights
    // the shader draws from are the heights bodies collide against, so the
    // surface can never disagree with what is on screen.
    if (const auto* terrain_comp = node.components.get<TerrainComponent>();
        terrain_comp != nullptr && terrain_comp->terrain != nullptr) {
        if (auto body_id = create_terrain_body(bi, impl_->filter_table,
                                               *terrain_comp->terrain, *spatial);
            !body_id.IsInvalid()) {
            impl_->object_to_body[node.id] = body_id;
            impl_->body_to_object[body_id.GetIndexAndSequenceNumber()] = node.id;
        } else {
            FJELL_CORE_ERROR("Failed to create terrain body for '{}'", node.name);
        }
        return;
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
    auto effective_shape = col.shape;
    if (col.inherit_from_mesh) {
        auto* mesh = node.components.get<MeshComponent>();
        if (mesh && mesh->model && collision_mesh_of(*mesh->model).valid()) {
            effective_shape = ColliderShape::Mesh;
        }
    }

    switch (effective_shape) {
        case ColliderShape::Box: {
            auto he = clamp_box_half_extents(col.half_extents * s);
            shape = new JPH::BoxShape(JPH::Vec3(he.x, he.y, he.z));
            break;
        }
        case ColliderShape::Sphere: {
            float r = clamp_positive(col.radius * std::max({s.x, s.y, s.z}));
            shape = new JPH::SphereShape(r);
            break;
        }
        case ColliderShape::Capsule: {
            float r = clamp_positive(col.radius * std::max(s.x, s.z));
            float half_h = clamp_positive(col.height * 0.5f * s.y);
            shape = new JPH::CapsuleShape(half_h, r);
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

            shape = build_authored_shape(primitives, collision, s, node.name,
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
        body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        body_settings.mMassPropertiesOverride.mMass = mass;
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
        create_body(*obj);
    }

    FJELL_CORE_INFO("Physics: created {} bodies, {} characters",
                     impl_->object_to_body.size(), impl_->characters.size());
}

// ── Step ────────────────────────────────────────────────────────────────

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
        create_body(*obj);
        set_linear_velocity(obj->id, velocity);
        set_angular_velocity(obj->id, angular);
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
    for (auto& entry : impl_->characters) {
        auto* node = scene.find_spatial(entry.object_id);
        if (!node) continue;
        auto* cc = node->components.get<CharacterComponent>();
        if (!cc) continue;

        // Apply gravity if in air
        glm::vec3 vel = cc->linear_velocity;
        if (cc->ground_state == CharacterGroundState::InAir ||
            cc->ground_state == CharacterGroundState::NotSupported) {
            vel.y += gravity.GetY() * dt;
        }
        cc->linear_velocity = vel;

        entry.character->SetLinearVelocity(to_jolt(vel));

        JPH::CharacterVirtual::ExtendedUpdateSettings update_settings;
        entry.character->ExtendedUpdate(dt, gravity, update_settings,
            impl_->physics_system->GetDefaultBroadPhaseLayerFilter(layers::MOVING),
            impl_->physics_system->GetDefaultLayerFilter(layers::MOVING),
            {},
            {},
            *impl_->temp_allocator);
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

        node->transform.position = to_glm(entry.character->GetPosition());

        cc->linear_velocity = to_glm(entry.character->GetLinearVelocity());
        cc->ground_velocity = to_glm(entry.character->GetGroundVelocity());
        cc->ground_normal = to_glm(entry.character->GetGroundNormal());

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
static void fill_hit(const JPH::PhysicsSystem& system,
                     const std::unordered_map<uint32_t, uint32_t>& body_to_object,
                     JPH::BodyID body_id, JPH::SubShapeID sub_shape,
                     JPH::RVec3Arg point, float distance, RaycastHit& out) {
    out.position = to_glm(point);
    out.distance = distance;

    auto it = body_to_object.find(body_id.GetIndexAndSequenceNumber());
    out.object_id = it != body_to_object.end() ? it->second : 0;

    JPH::BodyLockRead lock(system.GetBodyLockInterface(), body_id);
    if (lock.Succeeded()) {
        out.normal = to_glm(lock.GetBody().GetWorldSpaceSurfaceNormal(sub_shape, point));
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

    fill_hit(*impl_->physics_system, impl_->body_to_object, result.mBodyID,
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
        fill_hit(*impl_->physics_system, impl_->body_to_object, hit.mBodyID,
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
    fill_hit(*impl_->physics_system, impl_->body_to_object, hit.mBodyID2,
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
