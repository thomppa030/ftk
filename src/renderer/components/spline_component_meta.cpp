#include "renderer/components/spline_component.hpp"
#include "scene/component_registry.hpp"
#include "scene/node.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdio>

namespace fjell {

namespace {

const char* mode_name(SplineInterp m) {
    switch (m) {
        case SplineInterp::Linear:      return "Linear";
        case SplineInterp::CurveAuto:   return "Curve (Auto)";
        case SplineInterp::CurveUser:   return "Curve (User)";
        case SplineInterp::CurveBreak:  return "Curve (Break)";
        case SplineInterp::Constant:    return "Constant";
    }
    return "?";
}

bool mode_combo(const char* id, SplineInterp& mode) {
    bool changed = false;
    if (ImGui::BeginCombo(id, mode_name(mode))) {
        const SplineInterp all[] = {
            SplineInterp::Linear,
            SplineInterp::CurveAuto,
            SplineInterp::CurveUser,
            SplineInterp::CurveBreak,
            SplineInterp::Constant,
        };
        for (auto m : all) {
            const bool selected = (mode == m);
            if (ImGui::Selectable(mode_name(m), selected)) {
                mode = m;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void mark_dirty(SplineComponent* sp) {
    sp->dirty = true;
    sp->spline.auto_set_tangents();
    sp->spline.build_reparam_table(sp->reparam_samples);
}

} // namespace

static const bool registered = [] {
    ComponentRegistry::instance().register_component<SplineComponent>({
        .display_name = "Spline",
        .json_key = "spline",
        .color = {0.45f, 0.75f, 0.95f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& /*ctx*/) -> bool {
            auto* sp = static_cast<SplineComponent*>(ptr);
            auto& points = sp->spline.points;
            bool changed = false;

            ImGui::Text("Points: %zu", points.size());
            ImGui::SameLine();
            if (ImGui::Button("Add")) {
                const float key = points.empty() ? 0.0F : points.back().input_key + 1.0F;
                const glm::vec3 pos = points.empty()
                    ? glm::vec3(0.0F)
                    : points.back().value + glm::vec3(1.0F, 0.0F, 0.0F);
                sp->spline.add_point(key, pos);
                mark_dirty(sp);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear") && !points.empty()) {
                points.clear();
                sp->editor_selected_point = -1;
                mark_dirty(sp);
                changed = true;
            }

            bool closed = sp->spline.closed;
            if (ImGui::Checkbox("Closed", &closed)) {
                sp->spline.closed = closed;
                mark_dirty(sp);
                changed = true;
            }

            ImGui::Separator();
            ImGui::Text("Length: %.3f", sp->spline.total_length());

            ImGui::BeginChild("##SplinePoints", ImVec2(0, 260), true);
            int to_delete = -1;
            for (size_t i = 0; i < points.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                auto& p = points[i];
                const bool is_sel = (sp->editor_selected_point == static_cast<int>(i));
                char header[48];
                std::snprintf(header, sizeof(header), "%s#%zu  k=%.2f",
                              is_sel ? "* " : "  ", i, p.input_key);
                if (ImGui::SmallButton(is_sel ? "-" : "+")) {
                    sp->editor_selected_point = is_sel ? -1 : static_cast<int>(i);
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::TreeNode("node", "%s", header)) {
                    bool row_changed = false;

                    ImGui::SetNextItemWidth(80);
                    row_changed |= ImGui::DragFloat("Key", &p.input_key, 0.05F);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1);
                    row_changed |= mode_combo("##mode", p.interp_mode);

                    row_changed |= ImGui::DragFloat3("Position", &p.value.x, 0.05F);

                    if (p.interp_mode == SplineInterp::CurveUser
                     || p.interp_mode == SplineInterp::CurveBreak) {
                        row_changed |= ImGui::DragFloat3("Tangent In",  &p.arrive_tangent.x, 0.05F);
                        row_changed |= ImGui::DragFloat3("Tangent Out", &p.leave_tangent.x,  0.05F);
                        if (p.interp_mode == SplineInterp::CurveUser) {
                            // CurveUser is defined as symmetric — keep leave == arrive
                            // when either side is edited (arbitrary: leave follows arrive).
                            p.leave_tangent = p.arrive_tangent;
                        }
                    }

                    if (ImGui::Button("Delete")) to_delete = static_cast<int>(i);

                    if (row_changed) {
                        mark_dirty(sp);
                        changed = true;
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();

            if (to_delete >= 0) {
                sp->spline.remove_point(static_cast<size_t>(to_delete));
                if (sp->editor_selected_point == to_delete)      sp->editor_selected_point = -1;
                else if (sp->editor_selected_point > to_delete)  sp->editor_selected_point -= 1;
                mark_dirty(sp);
                changed = true;
            }

            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            auto& sp = node.components.add<SplineComponent>();
            // Seed with two points so the component is visibly editable.
            sp.spline.add_point(0.0F, glm::vec3(0.0F));
            sp.spline.add_point(1.0F, glm::vec3(1.0F, 0.0F, 0.0F));
            sp.spline.auto_set_tangents();
            sp.spline.build_reparam_table(sp.reparam_samples);
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
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
    return true;
}();

} // namespace fjell
