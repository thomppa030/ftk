#pragma once

#include "physics/collision_shape.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace fjell {

namespace gpu {
class RenderEncoder;
} // namespace gpu

class DebugDrawQueue;
class Scene;
struct CameraNode;

class DebugDraw {
public:
    /// Gathers the frame's debug primitives, which draw() then draws.
    void update(const Scene& scene, uint32_t selected_id = 0,
                bool show_colliders = true, DebugDrawQueue* script_queue = nullptr);

    /// Collision geometry to overlay on the next `update()`, in the mesh's
    /// local space. Submitted by the mesh editor while collision is being
    /// authored: the geometry belongs to the asset rather than to any node, so
    /// it has no component for the scene walk to find.
    ///
    /// A triangle mesh is drawn as wireframe edges, a point cloud (a convex
    /// hull, which stores no indices) as its points. Both are far denser than
    /// a primitive collider, so this is deliberately opt-in rather than part
    /// of `show_colliders` — one mesh under inspection can afford what a whole
    /// scene of them cannot.
    ///
    /// The lines are generated here and the caller's vectors are not retained,
    /// so submitting each frame costs one build and an editor that stops
    /// submitting simply stops being drawn.
    void submit_authored_collision(const std::vector<glm::vec3>& vertices,
                                   const std::vector<uint32_t>& indices);

    /// One authored collision shape, drawn in the mesh's local space. Submitted
    /// per shape while the mesh editor is open; `highlight` picks out the one
    /// being edited so a drag is visible against the others. A hull or
    /// triangle-mesh shape is outlined from the range of `geometry` it
    /// indexes, so a caller with only authored kinds may leave it empty.
    void submit_collision_primitive(const CollisionPrimitive& primitive, bool highlight,
                                    const CollisionMesh& geometry = {});

    /// Draws what the last `update()` gathered, through the debug pass's
    /// pipeline set on `pass`.
    void draw(gpu::RenderEncoder& pass) const;

    [[nodiscard]] bool empty() const { return draw_commands_.empty(); }

private:
    struct DrawCommand {
        glm::mat4 model;
        uint32_t first_vertex;
        uint32_t vertex_count;
        glm::vec4 color{0.0f, 1.0f, 0.0f, 1.0f}; // default green
    };

    void generate_box(const glm::vec3& half_extents);
    void generate_sphere(float radius, uint32_t segments = 24);
    void generate_capsule(float radius, float height, uint32_t segments = 16);
    void generate_frustum(const CameraNode& cam, float aspect);

    // Outline of a box, sphere or capsule primitive in its own frame, sized
    // the way the physics backend sizes it under a node scale: a box scales
    // per axis, a sphere by the largest axis, a capsule's radius by the
    // horizontal axes and its height by the vertical one. Geometry kinds add
    // nothing here; their lines come from collision_primitive_edges().
    void generate_primitive_outline(const CollisionPrimitive& primitive, const glm::vec3& scale);

    // Placement of one primitive under a node: the shape sits at its
    // authored position scaled with the node, in its authored rotation. The
    // scale itself is folded into the lines for the authored kinds and applied
    // here for the geometry kinds, whose cached outline is unscaled.
    [[nodiscard]] static glm::mat4 primitive_placement(const CollisionPrimitive& primitive,
                                                       const glm::vec3& scale);

    void add_line(const glm::vec3& a, const glm::vec3& b);
    void add_circle(const glm::vec3& center, const glm::vec3& axis_a,
                    const glm::vec3& axis_b, float radius, uint32_t segments);

    // Shapes submitted this frame, each with its own colour.
    struct AuthoredShape {
        glm::mat4 model;
        uint32_t first;
        uint32_t count;
        glm::vec4 color;
    };
    std::vector<AuthoredShape> authored_shapes_;

    // Line vertices built from the last submit_authored_collision() call,
    // prepended to each frame's buffer and cleared by update(). Owning them
    // means no editor's geometry can outlive the editor.
    std::vector<glm::vec3> authored_collision_lines_;

    std::vector<glm::vec3> vertices_;
    std::vector<DrawCommand> draw_commands_;
};

} // namespace fjell
