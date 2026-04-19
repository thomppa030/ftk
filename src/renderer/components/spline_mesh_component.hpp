#pragma once

#include "core/math/spline.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "scene/node_component.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

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
    // build the per-slice orthonormal frame. Z-up by default.
    glm::vec3 up_dir{0.0f, 0.0f, 1.0f};

    // Union of every per-segment deformed AABB, expressed in the node's
    // local space. Cached; recomputed when `dirty` is set.
    AABB local_bounds{};
    bool dirty{true};

    // Editor-only: currently manipulated control-point index. Mirrors the
    // field on SplineComponent so the same viewport gizmo logic applies.
    int editor_selected_point{-1};
};

} // namespace fjell
