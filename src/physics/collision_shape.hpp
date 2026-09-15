#pragma once

// Engine-side description of collision geometry, independent of any physics
// backend. Everything that needs a collider's shape — the physics backend,
// viewport picking, debug overlays — reads it through here rather than
// reaching into Model, so the geometry's origin can change without touching
// the consumers.

#include "renderer/resources/fjmesh.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fjell {

class Model;

// One shape in a mesh's collision list. Primitives are authored — a crate is a
// box, a barrel a capsule — while the geometry kinds are derived from the mesh
// and index into its collision arrays.
//
// A list of these replaces "the mesh has one collider shaped like its
// geometry". That framing cannot express a crate as a box, and it cannot make
// a concave object dynamic: no backend collides a moving body against a
// triangle mesh, but several hulls approximate one and move freely.
enum class CollisionPrimitiveKind : uint8_t {
    box = 0,
    sphere = 1,
    capsule = 2,
    convex_hull = 3,
    triangle_mesh = 4,
};

struct CollisionPrimitive {
    CollisionPrimitiveKind kind{CollisionPrimitiveKind::box};

    // Placement within the mesh's local space.
    glm::vec3 position{0.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};

    // Box half-extents; x is also the radius for sphere and capsule, and y the
    // capsule's full height. One vector rather than a union keeps the struct
    // trivially serializable and the inspector uniform.
    glm::vec3 extents{0.5F};

    // Joint this shape is parented to, for a shape authored on a skeleton.
    // -1 places it at the mesh origin, which is what every shape authored on a
    // mesh means, so the field is inert unless a skeleton put something in it.
    int32_t bone{-1};

    // Range into the mesh's collision arrays, for the geometry kinds. A hull
    // reads vertices only; a triangle mesh reads both.
    uint32_t first_vertex{0};
    uint32_t vertex_count{0};
    uint32_t first_index{0};
    uint32_t index_count{0};

    // Convex shapes can back a moving body; a triangle mesh cannot, in any
    // backend the engine is likely to use.
    [[nodiscard]] bool is_convex() const {
        return kind != CollisionPrimitiveKind::triangle_mesh;
    }
};

// A collision shape carried by a skeleton, parented to one of its joints.
// Authored on the skeleton rather than on a scene node, so every character
// using it inherits the shapes — the same way mesh collision is authored once
// on the .fjmesh and inherited by everything placed from it.
struct BoneCollider {
    CollisionPrimitive shape;
    std::string bone_name;  // resolved to shape.bone at load
};

// Collision geometry in the mesh's local space. A convex hull is stored as a
// bare point cloud — backends build the hull themselves — so `indices` is
// empty for it and non-empty for every triangle-based source. Whether the
// geometry came from the render mesh, a generated hull, or a separate proxy
// is the source's business, not the consumer's.
struct CollisionMesh {
    const std::vector<glm::vec3>* vertices{nullptr};
    const std::vector<uint32_t>* indices{nullptr};

    // How the geometry was produced. Backends need this to pick a shape
    // type: a hull can back a dynamic body, a triangle mesh cannot.
    FjMeshCollisionSource source{FjMeshCollisionSource::render_mesh};

    // Convex geometry can back a moving body; a triangle mesh is static-only
    // in every backend the engine is likely to use.
    [[nodiscard]] bool supports_dynamic() const {
        return source == FjMeshCollisionSource::convex_hull;
    }

    [[nodiscard]] bool valid() const {
        if (vertices == nullptr || vertices->empty()) return false;
        // A hull needs points only; anything else needs at least one triangle.
        if (supports_dynamic()) return vertices->size() >= 4;
        return indices != nullptr && indices->size() >= 3;
    }

    [[nodiscard]] uint32_t triangle_count() const {
        if (!valid() || indices == nullptr) return 0;
        return static_cast<uint32_t>(indices->size() / 3);
    }
};

// Resolves the collision geometry a model should collide with. Returns an
// invalid CollisionMesh when the model has none.
[[nodiscard]] CollisionMesh collision_mesh_of(const Model& model);

// The edges of the convex hull of `points`, as line-segment pairs in the same
// space the points are given in. Empty when fewer than four points are given.
// Coplanar input yields the flat outline rather than nothing, which is the
// honest picture of what such a collider would be.
//
// A hull collider stores only its points, since that is all a backend needs to
// build one. Anything that has to *show* the hull needs its shape, and
// deriving it here means the overlay draws the same solid the body will
// collide against rather than a second opinion about it.
[[nodiscard]] std::vector<glm::vec3> convex_hull_edges(const std::vector<glm::vec3>& points);

// The unique edges of a triangle list, as line-segment pairs. Interior edges
// are shared by two triangles, so emitting per triangle would draw most of a
// mesh twice. Indices outside `vertices` are skipped.
[[nodiscard]] std::vector<glm::vec3> triangle_mesh_edges(const std::vector<glm::vec3>& vertices,
                                                         std::span<const uint32_t> indices);

// The outline of one geometry-kind primitive — a hull's surface or a triangle
// mesh's wireframe — as line pairs in the mesh's local space, from the range
// of `geometry` the primitive indexes. Empty for the authored kinds, which are
// drawn from their extents, and when the geometry is missing.
[[nodiscard]] std::vector<glm::vec3> collision_primitive_edges(const CollisionPrimitive& primitive,
                                                               const CollisionMesh& geometry);

// The same outline for a primitive of a loaded model, built once per model
// and kept: a model's collision never changes after load, and the wireframe
// of a large triangle mesh is far too costly to rebuild every frame.
[[nodiscard]] const std::vector<glm::vec3>& collision_primitive_edges(const Model& model,
                                                                      size_t primitive_index);

} // namespace fjell
