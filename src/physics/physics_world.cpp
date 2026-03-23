#include "physics/physics_world.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "renderer/mesh_component.hpp"
#include "renderer/model.hpp"
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
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include <glm/gtc/quaternion.hpp>

#include <cstdarg>
#include <thread>
#include <unordered_set>

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
static constexpr JPH::ObjectLayer NON_MOVING = 0;
static constexpr JPH::ObjectLayer MOVING = 1;
static constexpr JPH::uint NUM_LAYERS = 2;
} // namespace layers

namespace broad_phase {
static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr JPH::uint NUM_LAYERS = 2;
} // namespace broad_phase

class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override {
        return broad_phase::NUM_LAYERS;
    }
    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == layers::NON_MOVING ? broad_phase::NON_MOVING : broad_phase::MOVING;
    }
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        if (layer == broad_phase::NON_MOVING) return "NON_MOVING";
        if (layer == broad_phase::MOVING) return "MOVING";
        return "UNKNOWN";
    }
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer obj_layer,
                                     JPH::BroadPhaseLayer bp_layer) const override {
        if (obj_layer == layers::NON_MOVING)
            return bp_layer == broad_phase::MOVING;
        return true;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == layers::NON_MOVING && b == layers::NON_MOVING)
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
    auto jq = JPH::Quat(q.x, q.y, q.z, q.w);
    return jq.IsNormalized() ? jq : jq.Normalized();
}

static glm::quat to_glm_quat(JPH::Quat q) {
    return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
}

// ── Contact listener ────────────────────────────────────────────────────

class FjellContactListener final : public JPH::ContactListener {
public:
    // Back-pointers for resolving body → object ID
    std::unordered_map<uint32_t, uint32_t>* body_to_object{nullptr};
    std::unordered_set<uint32_t>* trigger_bodies{nullptr};

    // Events collected this frame
    std::vector<std::pair<uint32_t, CollisionEvent>> collision_events;
    std::vector<std::pair<uint32_t, CollisionEvent>> trigger_events;

    void clear() {
        collision_events.clear();
        trigger_events.clear();
    }

    JPH::ValidateResult OnContactValidate(
        [[maybe_unused]] const JPH::Body& body1,
        [[maybe_unused]] const JPH::Body& body2,
        [[maybe_unused]] JPH::RVec3Arg base_offset,
        [[maybe_unused]] const JPH::CollideShapeResult& collision_result) override {
        // If either body is a trigger, allow contact but mark as sensor
        uint32_t key1 = body1.GetID().GetIndexAndSequenceNumber();
        uint32_t key2 = body2.GetID().GetIndexAndSequenceNumber();
        if (trigger_bodies->contains(key1) || trigger_bodies->contains(key2)) {
            return JPH::ValidateResult::AcceptContact;
        }
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(
        const JPH::Body& body1, const JPH::Body& body2,
        [[maybe_unused]] const JPH::ContactManifold& manifold,
        [[maybe_unused]] JPH::ContactSettings& settings) override {

        uint32_t key1 = body1.GetID().GetIndexAndSequenceNumber();
        uint32_t key2 = body2.GetID().GetIndexAndSequenceNumber();

        auto it1 = body_to_object->find(key1);
        auto it2 = body_to_object->find(key2);
        if (it1 == body_to_object->end() || it2 == body_to_object->end()) return;

        uint32_t obj1 = it1->second;
        uint32_t obj2 = it2->second;

        auto contact_point = to_glm(manifold.GetWorldSpaceContactPointOn1(0));
        auto contact_normal = to_glm(manifold.mWorldSpaceNormal);

        bool is_trigger = trigger_bodies->contains(key1) || trigger_bodies->contains(key2);

        if (is_trigger) {
            // Disable physical response for triggers
            settings.mCombinedFriction = 0.0f;
            settings.mCombinedRestitution = 0.0f;
            settings.mInvMassScale1 = 0.0f;
            settings.mInvMassScale2 = 0.0f;
            settings.mInvInertiaScale1 = 0.0f;
            settings.mInvInertiaScale2 = 0.0f;

            trigger_events.push_back({obj1, {obj2, contact_point, contact_normal}});
            trigger_events.push_back({obj2, {obj1, contact_point, -contact_normal}});
        } else {
            collision_events.push_back({obj1, {obj2, contact_point, contact_normal}});
            collision_events.push_back({obj2, {obj1, contact_point, -contact_normal}});
        }
    }
};

// ── Impl ────────────────────────────────────────────────────────────────

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
    std::unordered_set<uint32_t> trigger_bodies; // body keys that are triggers
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
    impl_->contact_listener.trigger_bodies = &impl_->trigger_bodies;
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

// ── Body creation ───────────────────────────────────────────────────────

void PhysicsWorld::create_bodies(Scene& scene) {
    auto& bi = impl_->physics_system->GetBodyInterface();

    for (auto& obj : scene.objects()) {
        const auto* col_comp = obj->components.get<ColliderComponent>();
        if (!col_comp) continue;
        auto* spatial = obj->as_spatial();
        if (!spatial) continue;

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

        switch (col.shape) {
            case ColliderShape::Box: {
                auto he = col.half_extents * s;
                shape = new JPH::BoxShape(JPH::Vec3(he.x, he.y, he.z));
                break;
            }
            case ColliderShape::Sphere: {
                float r = col.radius * std::max({s.x, s.y, s.z});
                shape = new JPH::SphereShape(r);
                break;
            }
            case ColliderShape::Capsule: {
                float r = col.radius * std::max(s.x, s.z);
                float half_h = col.height * 0.5f * s.y;
                shape = new JPH::CapsuleShape(half_h, r);
                break;
            }
            case ColliderShape::Mesh: {
                // Mesh colliders are static-only in Jolt
                if (rb.body_type != BodyType::Static) {
                    FJELL_CORE_WARN("Object '{}' has Mesh collider on non-static body — forcing static",
                                    obj->name);
                }
                // Mesh colliders require CPU vertex data from the model
                auto* mesh = obj->components.get<MeshComponent>();
                if (!mesh || !mesh->model || mesh->model->collision_vertices().empty()) {
                    FJELL_CORE_WARN("Object '{}' has Mesh collider but no collision mesh data",
                                    obj->name);
                    continue;
                }
                const auto& verts = mesh->model->collision_vertices();
                const auto& idxs = mesh->model->collision_indices();

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

        // Determine motion type (mesh colliders forced to static)
        auto effective_type = rb.body_type;
        if (col.shape == ColliderShape::Mesh && effective_type != BodyType::Static) {
            effective_type = BodyType::Static;
        }

        JPH::EMotionType motion;
        JPH::ObjectLayer layer;
        switch (effective_type) {
            case BodyType::Static:
                motion = JPH::EMotionType::Static;
                layer = layers::NON_MOVING;
                break;
            case BodyType::Kinematic:
                motion = JPH::EMotionType::Kinematic;
                layer = layers::NON_MOVING;
                break;
            case BodyType::Dynamic:
            default:
                motion = JPH::EMotionType::Dynamic;
                layer = layers::MOVING;
                break;
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

        if (rb.body_type == BodyType::Dynamic && rb.mass > 0.0f) {
            body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            body_settings.mMassPropertiesOverride.mMass = rb.mass;
        }

        auto body_id = bi.CreateAndAddBody(body_settings,
            rb.body_type == BodyType::Static ? JPH::EActivation::DontActivate
                                             : JPH::EActivation::Activate);

        if (body_id.IsInvalid()) {
            FJELL_CORE_ERROR("Failed to create physics body for '{}'", obj->name);
            continue;
        }

        impl_->object_to_body[obj->id] = body_id;
        impl_->body_to_object[body_id.GetIndexAndSequenceNumber()] = obj->id;

        if (col.is_trigger) {
            impl_->trigger_bodies.insert(body_id.GetIndexAndSequenceNumber());
        }
    }

    FJELL_CORE_INFO("Physics: created {} bodies", impl_->object_to_body.size());
}

// ── Step ────────────────────────────────────────────────────────────────

void PhysicsWorld::step(float dt, Scene& scene) {
    FJELL_PROFILE_SCOPE_N("physics_step");
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

        bi.SetPositionAndRotation(it->second,
                                  to_jolt(spatial->transform.position),
                                  to_jolt_quat(spatial->transform.rotation),
                                  JPH::EActivation::Activate);
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

const std::vector<std::pair<uint32_t, CollisionEvent>>& PhysicsWorld::collision_events() const {
    return impl_->contact_listener.collision_events;
}

const std::vector<std::pair<uint32_t, CollisionEvent>>& PhysicsWorld::trigger_events() const {
    return impl_->contact_listener.trigger_events;
}

} // namespace fjell
