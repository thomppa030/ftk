#include "renderer/components/spline_component.hpp"
#include "scene/component_registry.hpp"
#include "scene/node.hpp"
#include "scene/spline_editable.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>

namespace fjell {


static const bool registered = [] {
    ComponentRegistry::instance().register_component<SplineComponent>({
        .display_name = "Spline",
        .json_key = "spline",

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            auto& sp = node.components.add<SplineComponent>();
            // Seed with two points so the component is visibly editable.
            sp.spline.add_point(0.0F, glm::vec3(0.0F));
            sp.spline.add_point(1.0F, glm::vec3(1.0F, 0.0F, 0.0F));
            sp.spline.auto_set_tangents();
            sp.spline.build_reparam_table(sp.reparam_samples);
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* /*proj*/) {
            auto* sp = static_cast<const SplineComponent*>(ptr);
            out["closed"] = sp->spline.closed;
            out["reparam_samples"] = sp->reparam_samples;

            nlohmann::json arr = nlohmann::json::array();
            for (const auto& p : sp->spline.points) {
                nlohmann::json row;
                row["key"]     = p.input_key;
                row["value"]   = {p.value.x, p.value.y, p.value.z};
                row["arrive"]  = {p.arrive_tangent.x, p.arrive_tangent.y, p.arrive_tangent.z};
                row["leave"]   = {p.leave_tangent.x,  p.leave_tangent.y,  p.leave_tangent.z};
                row["mode"]    = static_cast<int>(p.interp_mode);
                arr.push_back(std::move(row));
            }
            out["points"] = std::move(arr);
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            if (!node.components.has<SplineComponent>()) {
                node.components.add<SplineComponent>();
            }
            auto* sp = node.components.get<SplineComponent>();
            if (!sp) return;

            sp->spline.points.clear();
            sp->spline.closed = j.value("closed", false);
            sp->reparam_samples = j.value("reparam_samples", 256U);

            if (j.contains("points") && j["points"].is_array()) {
                for (const auto& row : j["points"]) {
                    SplinePoint<glm::vec3> p;
                    p.input_key = row.value("key", 0.0F);
                    if (row.contains("value")) {
                        const auto& v = row["value"];
                        p.value = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
                    }
                    if (row.contains("arrive")) {
                        const auto& v = row["arrive"];
                        p.arrive_tangent = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
                    }
                    if (row.contains("leave")) {
                        const auto& v = row["leave"];
                        p.leave_tangent = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
                    }
                    p.interp_mode = static_cast<SplineInterp>(row.value("mode", 1));
                    sp->spline.points.push_back(p);
                }
            }
            sp->spline.auto_set_tangents();
            sp->spline.build_reparam_table(sp->reparam_samples);
            sp->dirty = true;
        },

        .script_type_name = {},
    });

    register_spline_editable<SplineComponent>(
        [](SplineComponent& c) -> Spline<glm::vec3>* { return &c.spline; },
        [](SplineComponent& c) -> int* { return &c.editor_selected_point; },
        [](SplineComponent& c) {
            c.spline.auto_set_tangents();
            c.spline.build_reparam_table(c.reparam_samples);
            c.dirty = true;
        });

    return true;
}();

} // namespace fjell
