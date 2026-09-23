#pragma once

#include "core/math/spline.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "scene/node_component.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell { struct GpuSplineSegment; }

namespace fjell {

class Model;
class Material;

/// Which axis of the source mesh runs along the spline.
/// Matches Unreal's ESplineMeshAxis. Encoded as uint32_t in the GPU SSBO.
enum class SplineMeshForward : uint8_t {
    X = 0,
    Y = 1,
    Z = 2,
};

/// Renders a single mesh deformed along a spline, one instance per
/// spline segment. Modeled after Unreal's USplineMeshComponent but with
/// the spline owned by this component rather than referenced externally.
///
/// Rendering path: reuses the instanced render pipeline. The per-segment
/// instance matrix is the owning node's world transform (same for every
/// segment — the spline's control points are already in that node's local
/// space). Per-segment deform data (endpoints, tangents, forward axis,
/// mesh half-span) lives in a separate SSBO consumed by the vertex stage.
struct SplineMeshComponent : NodeComponent {
    // Spline authoring (same shape as SplineComponent, but lives here so
    // the spline-mesh is self-contained — no sibling SplineComponent
    // required).
    Spline<glm::vec3> spline;
    uint32_t reparam_samples{256};

    // Mesh + material to deform along the spline.
    ModelHandle    model_handle;
    MaterialHandle material_handle;
    Model*         model{nullptr};
    Material*      material{nullptr};
    std::string    material_name;

    // Which axis of the source mesh points along the spline.
    SplineMeshForward forward{SplineMeshForward::X};

    // Reference "up" direction in the owning node's local space. Used to
    // build the per-slice orthonormal frame. Y-up matches Fjell's world up;
    // splines that lie on the ground plane (most common case) get a stable
    // frame across segments as the curve tangent sweeps through XZ.
    glm::vec3 up_dir{0.0F, 1.0F, 0.0F};

    // Union of every per-segment deformed AABB, expressed in the node's
    // local space. Cached; recomputed when `dirty` is set.
    AABB local_bounds{};
    bool dirty{true};

    // Cached render data rebuilt when `dirty` — one entry per spline segment.
    // `instance_transforms` mirrors the InstancedMesh convention: the gatherer
    // emits `instance_transforms->size()` instances per draw. For spline-mesh
    // all entries are identity — the node's world matrix already lives on
    // `DrawCommand::world_matrix`, and per-segment geometry is carried on the
    // `GpuSplineSegment` record. The shader reads `dd.model`, not the matrix
    // buffer, when `spline_segment_offset != ~0u`.
    std::vector<glm::mat4>        instance_transforms;
    std::vector<GpuSplineSegment> segments;

    // Editor-only: currently manipulated control-point index. Mirrors the
    // field on SplineComponent so the same viewport gizmo logic applies.
    int editor_selected_point{-1};
};

class Project;
class ResourceManager;

/// Loads `source` as the mesh laid along the spline. An empty source becomes
/// the default cube, written back, so clearing the slot leaves a visible,
/// editable shape rather than nothing drawn.
void load_spline_mesh_model(SplineMeshComponent& sm, std::string& source,
                            ResourceManager& resources, const Project* project);

/// Loads the component's material_name, or the default material when it is
/// empty or "default".
void load_spline_mesh_material(SplineMeshComponent& sm, ResourceManager& resources,
                               const Project* project);

} // namespace fjell
