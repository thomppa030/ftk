#include "renderer/debug_draw.hpp"
#include "renderer/components/arrow_component.hpp"
#include "renderer/gpu/buffer.hpp"
#include "physics/collision_shape.hpp"
#include "physics/physics_types.hpp"
#include "renderer/uniform_types.hpp"
#include "scripting/debug_draw_queue.hpp"
#include "core/profiler.hpp"
#include "scene/camera_node.hpp"
#include "scene/scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <numbers>
#include <unordered_set>
#include <utility>

namespace fjell {

DebugDraw::DebugDraw(VkDevice device, VmaAllocator allocator)
    : device_{device}, allocator_{allocator} {}

// ── Geometry helpers ────────────────────────────────────────────────────

void DebugDraw::add_line(const glm::vec3& a, const glm::vec3& b) {
    vertices_.push_back(a);
    vertices_.push_back(b);
}

void DebugDraw::add_circle(const glm::vec3& center, const glm::vec3& axis_a,
                           const glm::vec3& axis_b, float radius, uint32_t segments) {
    for (uint32_t i = 0; i < segments; ++i) {
        float a0 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(segments);
        float a1 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i + 1) / static_cast<float>(segments);
        auto p0 = center + (axis_a * std::cos(a0) + axis_b * std::sin(a0)) * radius;
        auto p1 = center + (axis_a * std::cos(a1) + axis_b * std::sin(a1)) * radius;
        add_line(p0, p1);
    }
}

void DebugDraw::generate_box(const glm::vec3& he) {
    // 8 corners
    glm::vec3 c[8] = {
        {-he.x, -he.y, -he.z}, { he.x, -he.y, -he.z},
        { he.x,  he.y, -he.z}, {-he.x,  he.y, -he.z},
        {-he.x, -he.y,  he.z}, { he.x, -he.y,  he.z},
        { he.x,  he.y,  he.z}, {-he.x,  he.y,  he.z},
    };
    // 12 edges
    add_line(c[0], c[1]); add_line(c[1], c[2]); add_line(c[2], c[3]); add_line(c[3], c[0]);
    add_line(c[4], c[5]); add_line(c[5], c[6]); add_line(c[6], c[7]); add_line(c[7], c[4]);
    add_line(c[0], c[4]); add_line(c[1], c[5]); add_line(c[2], c[6]); add_line(c[3], c[7]);
}

void DebugDraw::generate_sphere(float radius, uint32_t segments) {
    glm::vec3 origin{0.0f};
    add_circle(origin, {1, 0, 0}, {0, 1, 0}, radius, segments);
    add_circle(origin, {1, 0, 0}, {0, 0, 1}, radius, segments);
    add_circle(origin, {0, 1, 0}, {0, 0, 1}, radius, segments);
}

void DebugDraw::generate_capsule(float radius, float height, uint32_t segments) {
    float half_h = height * 0.5f;
    glm::vec3 top{0, half_h, 0};
    glm::vec3 bot{0, -half_h, 0};

    // Two circles at hemisphere centers
    add_circle(top, {1, 0, 0}, {0, 0, 1}, radius, segments);
    add_circle(bot, {1, 0, 0}, {0, 0, 1}, radius, segments);

    // Vertical lines connecting the circles
    for (uint32_t i = 0; i < 4; ++i) {
        float angle = std::numbers::pi_v<float> * 0.5f * static_cast<float>(i);
        float x = std::cos(angle) * radius;
        float z = std::sin(angle) * radius;
        add_line({x, half_h, z}, {x, -half_h, z});
    }

    // Top and bottom semicircles (two arcs each)
    uint32_t half_seg = segments / 2;
    for (uint32_t i = 0; i < half_seg; ++i) {
        float a0 = std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(half_seg);
        float a1 = std::numbers::pi_v<float> * static_cast<float>(i + 1) / static_cast<float>(half_seg);

        // Top hemisphere arcs (XY and ZY planes)
        add_line(top + glm::vec3{std::cos(a0) * radius, std::sin(a0) * radius, 0},
                 top + glm::vec3{std::cos(a1) * radius, std::sin(a1) * radius, 0});
        add_line(top + glm::vec3{0, std::sin(a0) * radius, std::cos(a0) * radius},
                 top + glm::vec3{0, std::sin(a1) * radius, std::cos(a1) * radius});

        // Bottom hemisphere arcs (mirror Y)
        add_line(bot + glm::vec3{std::cos(a0) * radius, -std::sin(a0) * radius, 0},
                 bot + glm::vec3{std::cos(a1) * radius, -std::sin(a1) * radius, 0});
        add_line(bot + glm::vec3{0, -std::sin(a0) * radius, std::cos(a0) * radius},
                 bot + glm::vec3{0, -std::sin(a1) * radius, std::cos(a1) * radius});
    }
}

void DebugDraw::generate_frustum(const CameraNode& cam, float aspect) {
    // Compute the 8 corners of the frustum in local camera space (looking down -Z)
    float n = cam.near_plane;
    float f = cam.far_plane;

    float near_h, near_w, far_h, far_w;
    if (cam.projection == CameraProjection::perspective) {
        float half_fov = glm::radians(cam.fov * 0.5f);
        near_h = n * std::tan(half_fov);
        near_w = near_h * aspect;
        far_h = f * std::tan(half_fov);
        far_w = far_h * aspect;
    } else {
        near_h = cam.ortho_size;
        near_w = cam.ortho_size * aspect;
        far_h = near_h;
        far_w = near_w;
    }

    // Near plane corners (camera looks down -Z)
    glm::vec3 ntl{-near_w,  near_h, -n};
    glm::vec3 ntr{ near_w,  near_h, -n};
    glm::vec3 nbl{-near_w, -near_h, -n};
    glm::vec3 nbr{ near_w, -near_h, -n};

    // Far plane corners
    glm::vec3 ftl{-far_w,  far_h, -f};
    glm::vec3 ftr{ far_w,  far_h, -f};
    glm::vec3 fbl{-far_w, -far_h, -f};
    glm::vec3 fbr{ far_w, -far_h, -f};

    // Near rect
    add_line(ntl, ntr); add_line(ntr, nbr);
    add_line(nbr, nbl); add_line(nbl, ntl);

    // Far rect
    add_line(ftl, ftr); add_line(ftr, fbr);
    add_line(fbr, fbl); add_line(fbl, ftl);

    // Connecting edges
    add_line(ntl, ftl); add_line(ntr, ftr);
    add_line(nbl, fbl); add_line(nbr, fbr);

    // Far plane diagonals (cross) for visibility at distance
    add_line(ftl, fbr); add_line(ftr, fbl);
}

// ── Update ──────────────────────────────────────────────────────────────

void DebugDraw::submit_authored_collision(const std::vector<glm::vec3>& vertices,
                                          const std::vector<uint32_t>& indices) {
    // Build into the member buffer by swapping it in as the scratch target,
    // so the line generation below can stay shared with the scene path.
    authored_collision_lines_.clear();
    std::swap(vertices_, authored_collision_lines_);
    generate_collision_wireframe(vertices, indices);
    std::swap(vertices_, authored_collision_lines_);
}

void DebugDraw::submit_collision_primitive(const CollisionPrimitive& prim, bool highlight) {
    uint32_t first = static_cast<uint32_t>(authored_collision_lines_.size());
    std::swap(vertices_, authored_collision_lines_);

    switch (prim.kind) {
        case CollisionPrimitiveKind::box:
            generate_box(prim.extents);
            break;
        case CollisionPrimitiveKind::sphere:
            generate_sphere(prim.extents.x);
            break;
        case CollisionPrimitiveKind::capsule:
            generate_capsule(prim.extents.x, prim.extents.y);
            break;
        default:
            // Hull and triangle-mesh shapes are drawn from the geometry they
            // index, which submit_authored_collision already covers.
            break;
    }

    std::swap(vertices_, authored_collision_lines_);
    uint32_t count = static_cast<uint32_t>(authored_collision_lines_.size()) - first;
    if (count == 0) return;

    auto model = glm::translate(glm::mat4(1.0f), prim.position) *
                 glm::mat4_cast(prim.rotation);
    authored_shapes_.push_back({model, first, count,
                                highlight ? glm::vec4{1.0f, 0.75f, 0.2f, 1.0f}
                                          : glm::vec4{0.25f, 0.85f, 0.4f, 1.0f}});
}

void DebugDraw::generate_collision_wireframe(const std::vector<glm::vec3>& vertices,
                                             const std::vector<uint32_t>& indices) {
    // A hull stores only the points it is built from, so its surface has to be
    // derived. Drawing the points themselves shows nothing about whether the
    // hull swallowed a concavity, which is the whole reason to look at it.
    if (indices.empty()) {
        auto edges = convex_hull_edges(vertices);
        for (size_t i = 0; i + 1 < edges.size(); i += 2) {
            add_line(edges[i], edges[i + 1]);
        }
        return;
    }

    // Interior edges are shared by two triangles, so emitting per-triangle
    // would draw most of the mesh twice.
    std::unordered_set<uint64_t> seen;
    seen.reserve(indices.size());
    auto edge = [&](uint32_t a, uint32_t b) {
        if (a > b) std::swap(a, b);
        uint64_t key = (static_cast<uint64_t>(a) << 32) | b;
        if (!seen.insert(key).second) return;
        if (a >= vertices.size() || b >= vertices.size()) return;
        add_line(vertices[a], vertices[b]);
    };

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        edge(indices[i], indices[i + 1]);
        edge(indices[i + 1], indices[i + 2]);
        edge(indices[i + 2], indices[i]);
    }
}

void DebugDraw::update(const Scene& scene, uint32_t frame_index, uint32_t selected_id,
                       bool show_colliders, DebugDrawQueue* script_queue) {
    FJELL_PROFILE_SCOPE_N("debug_draw_update");
    vertices_.clear();
    draw_commands_.clear();

    // Authored collision sits at the mesh's own origin, so it needs no node
    // transform — the preview places the mesh at identity. Consumed here, so
    // an editor that stops submitting stops being drawn.
    if (!authored_collision_lines_.empty()) {
        uint32_t base = static_cast<uint32_t>(vertices_.size());
        vertices_.insert(vertices_.end(), authored_collision_lines_.begin(),
                         authored_collision_lines_.end());

        if (authored_shapes_.empty()) {
            // Geometry submitted on its own, with no shape list over it.
            draw_commands_.push_back({glm::mat4(1.0f), base,
                                      static_cast<uint32_t>(authored_collision_lines_.size()),
                                      {0.25f, 0.85f, 0.4f, 1.0f}});
        } else {
            for (const auto& shape : authored_shapes_) {
                draw_commands_.push_back({shape.model, base + shape.first,
                                          shape.count, shape.color});
            }
        }
        authored_collision_lines_.clear();
        authored_shapes_.clear();
    }

    if (show_colliders) {
    for (const auto& obj : scene.objects()) {
        const auto* col_ptr = obj->components.get<ColliderComponent>();
        if (!col_ptr) continue;
        const auto* spatial = obj->as_spatial();
        if (!spatial) continue;
        const auto& col = *col_ptr;

        uint32_t first = static_cast<uint32_t>(vertices_.size());

        switch (col.shape) {
            case ColliderShape::Box:
                generate_box(col.half_extents * spatial->transform.scale);
                break;
            case ColliderShape::Sphere:
                generate_sphere(col.radius * std::max({spatial->transform.scale.x,
                                                        spatial->transform.scale.y,
                                                        spatial->transform.scale.z}));
                break;
            case ColliderShape::Capsule:
                generate_capsule(col.radius * std::max(spatial->transform.scale.x, spatial->transform.scale.z),
                                 col.height * spatial->transform.scale.y);
                break;
            case ColliderShape::Mesh:
                // Mesh collider wireframe not visualized (too many lines)
                break;
        }

        uint32_t count = static_cast<uint32_t>(vertices_.size()) - first;
        if (count == 0) continue;

        // Model matrix = object position + rotation + collider offset (rotated).
        // Scale is already baked into the shape dimensions, so strip it.
        glm::mat3 rot(glm::normalize(glm::vec3(spatial->world_matrix_[0])),
                      glm::normalize(glm::vec3(spatial->world_matrix_[1])),
                      glm::normalize(glm::vec3(spatial->world_matrix_[2])));
        glm::vec3 world_offset = rot * col.offset;
        glm::mat4 model(glm::vec4(rot[0], 0.0f),
                         glm::vec4(rot[1], 0.0f),
                         glm::vec4(rot[2], 0.0f),
                         glm::vec4(glm::vec3(spatial->world_matrix_[3]) + world_offset, 1.0f));
        draw_commands_.push_back({model, first, count});
    }

    // Draw capsule wireframe for nodes with CharacterComponent
    for (const auto& obj : scene.objects()) {
        const auto* cc = obj->components.get<CharacterComponent>();
        if (!cc) continue;
        const auto* spatial = obj->as_spatial();
        if (!spatial) continue;

        auto capsule = scaled_capsule(*cc, spatial->transform.scale);
        uint32_t first = static_cast<uint32_t>(vertices_.size());
        generate_capsule(capsule.radius, capsule.height);

        uint32_t count = static_cast<uint32_t>(vertices_.size()) - first;
        if (count == 0) continue;

        glm::mat4 model = glm::translate(glm::mat4(1.0f),
                                         spatial->transform.position + capsule.center);
        draw_commands_.push_back({model, first, count, {0.0f, 0.9f, 0.9f, 1.0f}});
    }

    // Draw arrows for nodes with ArrowComponent
    for (const auto& obj : scene.objects()) {
        const auto* arrow_ptr = obj->components.get<ArrowComponent>();
        if (!arrow_ptr) continue;
        const auto* spatial = obj->as_spatial();
        if (!spatial) continue;
        const auto& arr = *arrow_ptr;

        uint32_t first = static_cast<uint32_t>(vertices_.size());

        // Arrow shaft: local -Z direction (forward)
        glm::vec3 tip{0.0f, 0.0f, -arr.length};
        add_line({0.0f, 0.0f, 0.0f}, tip);

        // Arrowhead: two angled lines
        float head = arr.length * 0.2f;
        add_line(tip, {head * 0.3f, 0.0f, -arr.length + head});
        add_line(tip, {-head * 0.3f, 0.0f, -arr.length + head});
        add_line(tip, {0.0f, head * 0.3f, -arr.length + head});
        add_line(tip, {0.0f, -head * 0.3f, -arr.length + head});

        uint32_t count = static_cast<uint32_t>(vertices_.size()) - first;
        draw_commands_.push_back({spatial->world_matrix_, first, count});
    }
    } // show_colliders

    // Camera frustum gizmo for selected camera nodes (always shown)
    if (selected_id != 0) {
        const auto* obj = scene.find(selected_id);
        if (obj) {
            const auto* cam = dynamic_cast<const CameraNode*>(obj);
            if (cam) {
                uint32_t first = static_cast<uint32_t>(vertices_.size());
                generate_frustum(*cam, 16.0f / 9.0f);
                uint32_t count = static_cast<uint32_t>(vertices_.size()) - first;
                if (count > 0) {
                    draw_commands_.push_back(
                        {cam->world_matrix_, first, count, {0.9f, 0.8f, 0.3f, 1.0f}});
                }
            }
        }
    }

    // Script-submitted debug primitives
    if (script_queue) {
        constexpr float POINT_SIZE = 0.05f;

        for (const auto& p : script_queue->primitives()) {
            uint32_t first = static_cast<uint32_t>(vertices_.size());

            switch (p.type) {
                case DebugPrimitive::Type::Line:
                    add_line(p.a, p.b);
                    break;

                case DebugPrimitive::Type::Box:
                    generate_box(p.b); // b = half_extents
                    break;

                case DebugPrimitive::Type::Sphere:
                    generate_sphere(p.radius);
                    break;

                case DebugPrimitive::Type::Point:
                    // Small 3-axis cross
                    add_line({-POINT_SIZE, 0, 0}, {POINT_SIZE, 0, 0});
                    add_line({0, -POINT_SIZE, 0}, {0, POINT_SIZE, 0});
                    add_line({0, 0, -POINT_SIZE}, {0, 0, POINT_SIZE});
                    break;
            }

            uint32_t count = static_cast<uint32_t>(vertices_.size()) - first;
            if (count == 0) continue;

            // For line, model is identity positioned at origin (vertices are world-space).
            // For box/sphere/point, model translates to center position.
            glm::mat4 model = glm::translate(glm::mat4(1.0f), p.a);
            if (p.type == DebugPrimitive::Type::Line) {
                model = glm::mat4(1.0f); // line vertices are already world-space
            }
            draw_commands_.push_back({model, first, count, p.color});
        }
    }

    if (vertices_.empty()) return;

    // Upload to GPU — write into this frame's slot only. Its fence was
    // waited on before record_frame(), so destroying/replacing it here
    // cannot race with an in-flight submission on another slot.
    auto& buf = vertex_buffers_[frame_index];
    auto& cap = buffer_capacities_[frame_index];
    VkDeviceSize required = vertices_.size() * sizeof(glm::vec3);
    if (!buf || required > cap) {
        // Round up to avoid frequent reallocations
        cap = std::max(required, VkDeviceSize{4096});
        buf = std::make_unique<Buffer>(
            allocator_, cap,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        buf->map();
    }
    buf->write(vertices_.data(), required);
}

// ── Draw ────────────────────────────────────────────────────────────────

void DebugDraw::draw(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frame_index) const {
    const auto& buf = vertex_buffers_[frame_index];
    if (!buf || draw_commands_.empty()) return;

    VkBuffer vb = buf->handle();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);

    for (const auto& dc : draw_commands_) {
        DebugPushConstants push{};
        push.model = dc.model;
        push.color = dc.color;

        vkCmdPushConstants(cmd, layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(DebugPushConstants), &push);
        vkCmdDraw(cmd, dc.vertex_count, 1, dc.first_vertex, 0);
    }
}

} // namespace fjell
