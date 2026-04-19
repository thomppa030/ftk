#pragma once

#include "renderer/render_queue.hpp"
#include "scene/node.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace fjell {

struct Node3D;

/// One provider emits a DrawCommand for a component type that renders
/// through the instanced path (InstancedMesh, SplineMesh, later
/// SplineInstancedMesh). Registered once at startup from each
/// `*_meta.cpp`; probed per-node during the gather pass.
///
/// The probe is allowed to mutate the component (e.g. rebuild cached
/// `local_bounds` when dirty) — it runs once per object per frame.
///
/// Returns std::nullopt when the component is missing or not ready to
/// render (no model, no instances).
using InstanceProviderProbe =
    std::function<std::optional<DrawCommand>(Node& node, const Node3D& spatial)>;

class InstanceProviderRegistry {
public:
    static InstanceProviderRegistry& instance();

    void add(InstanceProviderProbe probe) { probes_.push_back(std::move(probe)); }

    [[nodiscard]] const std::vector<InstanceProviderProbe>& probes() const {
        return probes_;
    }

private:
    std::vector<InstanceProviderProbe> probes_;
};

} // namespace fjell
