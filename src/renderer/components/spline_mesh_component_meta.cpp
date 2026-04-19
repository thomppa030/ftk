#include "renderer/components/spline_mesh_component.hpp"
#include "scene/component_registry.hpp"
#include "scene/node.hpp"
#include "scene/spline_editable.hpp"
#include "ui/asset_ref_widget.hpp"
#include "core/base_path.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/material.hpp"
#include "scripting/asset_type_registry.hpp"

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

const char* forward_name(SplineMeshForward f) {
    switch (f) {
        case SplineMeshForward::X: return "X";
        case SplineMeshForward::Y: return "Y";
        case SplineMeshForward::Z: return "Z";
    }
    return "?";
}

void mark_dirty(SplineMeshComponent* sm) {
    sm->dirty = true;
    sm->spline.auto_set_tangents();
    sm->spline.build_reparam_table(sm->reparam_samples);
}

void resolve_model(SplineMeshComponent* sm, const std::string& source, const InspectorContext& ctx) {
    if (!ctx.resource_manager || !ctx.project_mgr || source.empty()) return;
    auto resolved = base_path::resolve_asset(source, ctx.project_mgr->active());
    auto result = ctx.resource_manager->load_model(resolved);
    if (result) {
        sm->model_handle = *result;
        sm->model = ctx.resource_manager->get(sm->model_handle);
        sm->dirty = true;
    }
}

void resolve_material(SplineMeshComponent* sm, const InspectorContext& ctx) {
    if (!ctx.resource_manager) return;
    if (sm->material_name.empty()) sm->material_name = "default";
    if (sm->material_name != "default" && ctx.project_mgr) {
        auto resolved = base_path::resolve_asset(sm->material_name, ctx.project_mgr->active());
        auto result = ctx.resource_manager->load_material_asset(resolved, ctx.project_mgr->active());
        if (result) {
            sm->material_handle = *result;
            sm->material = ctx.resource_manager->get(sm->material_handle);
        }
    } else if (sm->material_name == "default") {
        auto tex = ctx.resource_manager->default_white_handle();
        sm->material_handle = ctx.resource_manager->create_material("default", tex);
        sm->material = ctx.resource_manager->get(sm->material_handle);
    }
}

} // namespace

static const bool registered = [] {
    ComponentRegistry::instance().register_component<SplineMeshComponent>({
        .display_name = "Spline Mesh",
        .json_key = "spline_mesh",
        .color = {0.35f, 0.85f, 0.70f, 1.0f},

        .draw = [](Node3D& node, void* ptr, const InspectorContext& ctx) -> bool {
            auto* sm = static_cast<SplineMeshComponent*>(ptr);
            bool changed = false;

            // ── Mesh + Material ──
            static const std::string model_exts[] = {".fjmesh"};
            static const std::string material_exts[] = {".fjmat"};

            std::string model_path = node.source;
            if (asset_ref_field({.label = "Model", .id = "##SMeshModel", .extensions = model_exts, .ctx = &ctx}, model_path)) {
                node.source = model_path;
                resolve_model(sm, model_path, ctx);
                changed = true;
            }

            const auto* mat_type = AssetTypeRegistry::instance().find_by_name("Material");
            if (asset_ref_field({.label = "Material", .id = "##SMeshMaterial", .extensions = material_exts, .ctx = &ctx, .asset_type = mat_type}, sm->material_name)) {
                resolve_material(sm, ctx);
                changed = true;
            }

            // ── Forward axis + up ──
            if (ImGui::BeginCombo("Forward", forward_name(sm->forward))) {
                const SplineMeshForward all[] = {
                    SplineMeshForward::X, SplineMeshForward::Y, SplineMeshForward::Z
                };
                for (auto f : all) {
                    const bool selected = (sm->forward == f);
                    if (ImGui::Selectable(forward_name(f), selected)) {
                        sm->forward = f;
                        sm->dirty = true;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::DragFloat3("Up", &sm->up_dir.x, 0.05F)) {
                sm->dirty = true;
                changed = true;
            }

            ImGui::Separator();

            // ── Spline authoring (mirrors SplineComponent) ──
            auto& points = sm->spline.points;
            ImGui::Text("Points: %zu", points.size());
            ImGui::SameLine();
            if (ImGui::Button("Add")) {
                const float key = points.empty() ? 0.0F : points.back().input_key + 1.0F;
                const glm::vec3 pos = points.empty()
                    ? glm::vec3(0.0F)
                    : points.back().value + glm::vec3(1.0F, 0.0F, 0.0F);
                sm->spline.add_point(key, pos);
                mark_dirty(sm);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear") && !points.empty()) {
                points.clear();
                sm->editor_selected_point = -1;
                mark_dirty(sm);
                changed = true;
            }

            bool closed = sm->spline.closed;
            if (ImGui::Checkbox("Closed", &closed)) {
                sm->spline.closed = closed;
                mark_dirty(sm);
                changed = true;
            }

            ImGui::Text("Length: %.3f   Segments: %zu",
                        sm->spline.total_length(),
                        points.size() > 1 ? points.size() - 1 + (closed ? 1 : 0) : 0);

            ImGui::BeginChild("##SplineMeshPoints", ImVec2(0, 240), true);
            int to_delete = -1;
            for (size_t i = 0; i < points.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                auto& p = points[i];
                const bool is_sel = (sm->editor_selected_point == static_cast<int>(i));
                char header[48];
                std::snprintf(header, sizeof(header), "%s#%zu  k=%.2f",
                              is_sel ? "* " : "  ", i, p.input_key);
                if (ImGui::SmallButton(is_sel ? "-" : "+")) {
                    sm->editor_selected_point = is_sel ? -1 : static_cast<int>(i);
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
                            p.leave_tangent = p.arrive_tangent;
                        }
                    }

                    if (ImGui::Button("Delete")) to_delete = static_cast<int>(i);

                    if (row_changed) {
                        mark_dirty(sm);
                        changed = true;
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();

            if (to_delete >= 0) {
                sm->spline.remove_point(static_cast<size_t>(to_delete));
                if (sm->editor_selected_point == to_delete)     sm->editor_selected_point = -1;
                else if (sm->editor_selected_point > to_delete) sm->editor_selected_point -= 1;
                mark_dirty(sm);
                changed = true;
            }

            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            auto& sm = node.components.add<SplineMeshComponent>();
            sm.spline.add_point(0.0F, glm::vec3(0.0F));
            sm.spline.add_point(1.0F, glm::vec3(2.0F, 0.0F, 0.0F));
            sm.spline.auto_set_tangents();
            sm.spline.build_reparam_table(sm.reparam_samples);
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* sm = static_cast<const SplineMeshComponent*>(ptr);
            out["material"]       = sm->material_name;
            out["forward"]        = static_cast<int>(sm->forward);
            out["up"]             = {sm->up_dir.x, sm->up_dir.y, sm->up_dir.z};
            out["closed"]         = sm->spline.closed;
            out["reparam_samples"] = sm->reparam_samples;

            nlohmann::json arr = nlohmann::json::array();
            for (const auto& p : sm->spline.points) {
                nlohmann::json row;
                row["key"]    = p.input_key;
                row["value"]  = {p.value.x, p.value.y, p.value.z};
                row["arrive"] = {p.arrive_tangent.x, p.arrive_tangent.y, p.arrive_tangent.z};
                row["leave"]  = {p.leave_tangent.x,  p.leave_tangent.y,  p.leave_tangent.z};
                row["mode"]   = static_cast<int>(p.interp_mode);
                arr.push_back(std::move(row));
            }
            out["points"] = std::move(arr);
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& res, const Project* proj) {
            if (!node.components.has<SplineMeshComponent>()) {
                node.components.add<SplineMeshComponent>();
            }
            auto* sm = node.components.get<SplineMeshComponent>();
            if (!sm) return;

            sm->material_name   = j.value("material", "default");
            sm->forward         = static_cast<SplineMeshForward>(j.value("forward", 0));
            sm->reparam_samples = j.value("reparam_samples", 256U);
            sm->spline.closed   = j.value("closed", false);

            if (j.contains("up") && j["up"].is_array() && j["up"].size() == 3) {
                sm->up_dir = {j["up"][0].get<float>(),
                              j["up"][1].get<float>(),
                              j["up"][2].get<float>()};
            }

            if (!node.source.empty()) {
                auto resolved = base_path::resolve_asset(node.source, proj);
                auto result = res.load_model(resolved);
                if (result) {
                    sm->model_handle = *result;
                    sm->model = res.get(sm->model_handle);
                }
            }

            if (sm->material_name != "default" && sm->material_name.ends_with(".fjmat")) {
                auto resolved = base_path::resolve_asset(sm->material_name, proj);
                auto result = res.load_material_asset(resolved, proj);
                if (result) {
                    sm->material_handle = *result;
                    sm->material = res.get(sm->material_handle);
                }
            } else {
                auto tex = res.default_white_handle();
                sm->material_handle = res.create_material("default", tex);
                sm->material = res.get(sm->material_handle);
            }

            sm->spline.points.clear();
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
                    sm->spline.points.push_back(p);
                }
            }
            sm->spline.auto_set_tangents();
            sm->spline.build_reparam_table(sm->reparam_samples);
            sm->dirty = true;
        },

        .script_type_name = {},
    });

    register_spline_editable<SplineMeshComponent>(
        [](SplineMeshComponent& c) -> Spline<glm::vec3>* { return &c.spline; },
        [](SplineMeshComponent& c) -> int* { return &c.editor_selected_point; },
        [](SplineMeshComponent& c) {
            c.spline.auto_set_tangents();
            c.spline.build_reparam_table(c.reparam_samples);
            c.dirty = true;
        });

    return true;
}();

} // namespace fjell
