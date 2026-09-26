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

/// A probe and the component it draws, so the gather pass visits only the
/// nodes that carry one.
struct InstanceProvider {
    ComponentTypeId component;
    InstanceProviderProbe probe;
};

class InstanceProviderRegistry {
public:
    static InstanceProviderRegistry& instance();

    void add(ComponentTypeId component, InstanceProviderProbe probe) {
        providers_.push_back({component, std::move(probe)});
    }

    [[nodiscard]] const std::vector<InstanceProvider>& providers() const {
        return providers_;
    }

private:
    std::vector<InstanceProvider> providers_;
};

} // namespace fjell
