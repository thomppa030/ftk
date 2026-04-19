#pragma once

#include "core/math/spline.hpp"
#include "scene/node.hpp"

#include <functional>
#include <vector>

namespace fjell {

/// Adapter view of any component that carries an editable 3D spline plus
/// an editor-selected-point index. Registered once per component type so
/// the scene editor's viewport authoring (polyline preview, control-point
/// handles, click-to-pick, gizmo) works uniformly for every spline-bearing
/// component without hardcoding the list.
///
/// `on_edited` is called after the viewport authoring has mutated the
/// spline (via gizmo drag). Components use it to re-run auto tangents,
/// rebuild reparam tables, and flip their own dirty flags.
struct SplineEditable {
    Spline<glm::vec3>*     spline{nullptr};
    int*                   selected_point{nullptr};
    std::function<void()>  on_edited;
};

/// Probes a node for any registered spline-bearing component. Returns the
/// first hit; nodes with multiple spline components are edge-case and not
/// supported (same as nodes with multiple meshes today).
///
/// The probe is populated at component-registration time by calling
/// register_spline_editable<T>() — typically from each component's
/// `*_meta.cpp`.
class SplineEditableRegistry {
public:
    static SplineEditableRegistry& instance();

    using Probe = std::function<SplineEditable(Node&)>;

    void add_probe(Probe probe) { probes_.push_back(std::move(probe)); }

    /// Returns the first matching adapter, or {nullptr, nullptr} if the
    /// node has no spline-bearing component.
    [[nodiscard]] SplineEditable probe(Node& node) const {
        for (const auto& p : probes_) {
            auto r = p(node);
            if (r.spline != nullptr) return r;
        }
        return {};
    }

private:
    std::vector<Probe> probes_;
};

/// Convenience wrapper — call from each component's `*_meta.cpp`
/// static-initializer alongside `register_component<T>()`. `on_edited`
/// is invoked after the viewport authoring has mutated the component's
/// spline, so the component can refresh its derived state.
template <typename T>
void register_spline_editable(Spline<glm::vec3>* (*get_spline)(T&),
                              int* (*get_selected)(T&),
                              void (*on_edited)(T&)) {
    SplineEditableRegistry::instance().add_probe([=](Node& node) -> SplineEditable {
        auto* c = node.components.get<T>();
        if (!c) return {};
        SplineEditable e;
        e.spline         = get_spline(*c);
        e.selected_point = get_selected(*c);
        e.on_edited      = [c, on_edited]() { on_edited(*c); };
        return e;
    });
}

} // namespace fjell
