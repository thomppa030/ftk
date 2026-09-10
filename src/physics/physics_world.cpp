#include "physics/physics_world.hpp"
#include "physics/collision_shape.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "renderer/components/mesh_component.hpp"
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
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include <glm/gtc/quaternion.hpp>

#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <cstdarg>
#include <mutex>
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
static constexpr JPH::uint NUM_LAYERS = 3;
} // namespace layers

namespace broad_phase {
static constexpr JPH::BroadPhaseLayer STATIC(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr JPH::BroadPhaseLayer SENSOR(2);
static constexpr JPH::uint NUM_LAYERS = 3;
} // namespace broad_phase

class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override {
        return broad_phase::NUM_LAYERS;
    }
    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
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
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer obj_layer,
                                     JPH::BroadPhaseLayer bp_layer) const override {
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
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        // Static vs static = no collision
        if (a == layers::STATIC && b == layers::STATIC)
            return false;
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

    BPLayerInterface bp_layer_interface;
    ObjectVsBroadPhaseFilter obj_vs_bp_filter;
    ObjectLayerPairFilter obj_pair_filter;
    FjellContactListener contact_listener;

    // Bidirectional maps: Fjell object_id <-> Jolt BodyID (stored as uint32)
    std::unordered_map<uint32_t, JPH::BodyID> object_to_body;
    std::unordered_map<uint32_t, uint32_t> body_to_object; // body.GetIndexAndSequenceNumber() -> object_id

    // Character controllers
    std::vector<CharacterEntry> characters;
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

static float clamp_positive(float v) {
    return std::max(v, MIN_DIMENSION);
}

static glm::vec3 clamp_positive(const glm::vec3& v) {
    return {std::max(v.x, MIN_DIMENSION), std::max(v.y, MIN_DIMENSION), std::max(v.z, MIN_DIMENSION)};
}

// ── Body creation ───────────────────────────────────────────────────────

void PhysicsWorld::create_bodies(Scene& scene) {
    auto& bi = impl_->physics_system->GetBodyInterface();

    for (auto& obj : scene.objects()) {
        auto* spatial = obj->as_spatial();
        if (!spatial) continue;

        // Character controller — handled in Step 5 section below
        auto* char_comp = obj->components.get<CharacterComponent>();
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

            impl_->characters.push_back({character, obj->id});

            // Register inner body for contact resolution
            auto inner_id = character->GetInnerBodyID();
            if (!inner_id.IsInvalid()) {
                impl_->body_to_object[inner_id.GetIndexAndSequenceNumber()] = obj->id;
            }
            continue;
        }

        const auto* col_comp = obj->components.get<ColliderComponent>();
        if (!col_comp) continue;

        // Default to static if no rigidbody descriptor
        static const auto default_static = [] {
            RigidbodyComponent rb;
            rb.body_type = BodyType::Static;
            return rb;
        }();
        const auto* rb_comp = obj->components.get<RigidbodyComponent>();
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
            auto* mesh = obj->components.get<MeshComponent>();
            if (mesh && mesh->model && collision_mesh_of(*mesh->model).valid()) {
                effective_shape = ColliderShape::Mesh;
            }
        }

        switch (effective_shape) {
            case ColliderShape::Box: {
                auto he = clamp_positive(col.half_extents * s);
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
                auto* mesh = obj->components.get<MeshComponent>();
                if (!mesh || !mesh->model) {
                    FJELL_CORE_WARN("Object '{}' has Mesh collider but no mesh", obj->name);
                    continue;
                }
                auto collision = collision_mesh_of(*mesh->model);
                if (!collision.valid()) {
                    FJELL_CORE_WARN("Object '{}' has Mesh collider but no collision mesh data",
                                    obj->name);
                    continue;
                }

                const auto& verts = *collision.vertices;
                const auto& idxs = *collision.indices;

                mesh_supports_dynamic = collision.supports_dynamic();
                if (mesh_supports_dynamic) {
                    JPH::Array<JPH::Vec3> points;
                    points.reserve(verts.size());
                    for (const auto& v : verts) {
                        auto p = v * s;
                        points.push_back(JPH::Vec3(p.x, p.y, p.z));
                    }

                    auto hull_result = JPH::ConvexHullShapeSettings(points).Create();
                    if (hull_result.HasError()) {
                        FJELL_CORE_ERROR("Failed to create hull collider for '{}': {}",
                                         obj->name, hull_result.GetError().c_str());
                        continue;
                    }
                    shape = hull_result.Get();
                    break;
                }

                // Triangle meshes can't back a moving body in Jolt.
                if (rb.body_type != BodyType::Static) {
                    FJELL_CORE_WARN("Object '{}' has a triangle-mesh collider on a non-static "
                                    "body — forcing static. Use a convex hull to keep it moving.",
                                    obj->name);
                }

                JPH::TriangleList triangles;
                triangles.reserve(idxs.size() / 3);
                for (size_t i = 0; i + 2 < idxs.size(); i += 3) {
                    auto v0 = verts[idxs[i]] * s;
                    auto v1 = verts[idxs[i + 1]] * s;
                    auto v2 = verts[idxs[i + 2]] * s;
                    triangles.push_back(JPH::Triangle(
                        JPH::Float3(v0.x, v0.y, v0.z),
                        JPH::Float3(v1.x, v1.y, v1.z),
                        JPH::Float3(v2.x, v2.y, v2.z)));
                }

                auto mesh_settings = JPH::MeshShapeSettings(triangles);
                auto mesh_result = mesh_settings.Create();
                if (mesh_result.HasError()) {
                    FJELL_CORE_ERROR("Failed to create mesh collider for '{}': {}",
                                     obj->name, mesh_result.GetError().c_str());
                    continue;
                }
                shape = mesh_result.Get();
                break;
            }
        }

        if (!shape) continue;

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

        // Sensors use SENSOR layer
        bool is_sensor = col.is_trigger;

        JPH::EMotionType motion;
        JPH::ObjectLayer layer;
        if (is_sensor) {
            // Sensors are kinematic so they can be moved and generate contacts
            motion = (effective_type == BodyType::Static)
                ? JPH::EMotionType::Static : JPH::EMotionType::Kinematic;
            layer = layers::SENSOR;
        } else {
            switch (effective_type) {
                case BodyType::Static:
                    motion = JPH::EMotionType::Static;
                    layer = layers::STATIC;
                    break;
                case BodyType::Kinematic:
                    motion = JPH::EMotionType::Kinematic;
                    layer = layers::MOVING;
                    break;
                case BodyType::Dynamic:
                default:
                    motion = JPH::EMotionType::Dynamic;
                    layer = layers::MOVING;
                    break;
            }
        }

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
            FJELL_CORE_ERROR("Failed to create physics body for '{}'", obj->name);
            continue;
        }

        impl_->object_to_body[obj->id] = body_id;
        impl_->body_to_object[body_id.GetIndexAndSequenceNumber()] = obj->id;
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

void PhysicsWorld::remove_body(uint32_t object_id) {
    auto it = impl_->object_to_body.find(object_id);
    if (it == impl_->object_to_body.end()) return;

    auto& bi = impl_->physics_system->GetBodyInterface();
    bi.RemoveBody(it->second);
    bi.DestroyBody(it->second);

    impl_->body_to_object.erase(it->second.GetIndexAndSequenceNumber());
    impl_->object_to_body.erase(it);
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

bool PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction,
                           float max_distance, RaycastHit& out_hit) const {
    auto dir_norm = glm::normalize(direction);
    JPH::RRayCast ray(to_jolt(origin), to_jolt(dir_norm * max_distance));
    JPH::RayCastResult result;

    if (!impl_->physics_system->GetNarrowPhaseQuery().CastRay(ray, result)) {
        return false;
    }

    auto hit_point = ray.GetPointOnRay(result.mFraction);
    out_hit.position = to_glm(hit_point);
    out_hit.distance = max_distance * result.mFraction;

    // Look up object ID from body
    auto body_it = impl_->body_to_object.find(result.mBodyID.GetIndexAndSequenceNumber());
    if (body_it != impl_->body_to_object.end()) {
        out_hit.object_id = body_it->second;
    }

    // Get surface normal
    JPH::BodyLockRead lock(impl_->physics_system->GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) {
        auto normal = lock.GetBody().GetWorldSpaceSurfaceNormal(
            result.mSubShapeID2, hit_point);
        out_hit.normal = to_glm(normal);
    }

    return true;
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

} // namespace fjell
