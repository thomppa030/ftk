#pragma once

#include "core/math/spline.hpp"
#include "scene/node_component.hpp"

#include <glm/glm.hpp>

namespace fjell {

/// A 3D spline authored in local space. Control points are nodes on the
/// Hermite spline from `core/math/spline.hpp`; evaluation, arc-length, and
/// tangent queries come from there.
///
/// Coordinates are local to the owning node — apply the node's world
/// transform to get world-space positions.
struct SplineComponent : NodeComponent {
    Spline<glm::vec3> spline;

    /// Cached reparam sample count. Rebuilt when the spline is edited.
    uint32_t reparam_samples{256};

    /// Set to true by the editor whenever points or modes change.
    /// Consumers (chain sim, renderer) clear it after reacting.
    bool dirty{true};

    /// Editor-only: index of the control point currently being manipulated
    /// by the viewport gizmo. -1 when no point is selected. Not serialized.
    int editor_selected_point{-1};
};

} // namespace fjell
