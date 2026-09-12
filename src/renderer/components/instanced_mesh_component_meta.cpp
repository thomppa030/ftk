#include "scene/component_registry.hpp"
#include "renderer/components/instanced_mesh_component.hpp"
#include "renderer/instance_provider.hpp"
#include "renderer/resources/model.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"
#include "core/base_path.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/material.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
#include <random>
#include <unordered_map>

namespace fjell {

namespace {

void resolve_model_for_inst(InstancedMeshComponent* inst, const std::string& source, const InspectorContext& ctx) {
    if (!ctx.resource_manager || !ctx.project_mgr || source.empty()) return;
    auto resolved = base_path::resolve_asset(source, ctx.project_mgr->active());
    auto result = ctx.resource_manager->load_model(resolved);
    if (result) {
        inst->model_handle = *result;
        inst->model = ctx.resource_manager->get(inst->model_handle);
        inst->dirty = true;
    }
}

void resolve_material_for_inst(InstancedMeshComponent* inst, const InspectorContext& ctx) {
    if (!ctx.resource_manager) return;
    if (inst->material_name.empty()) inst->material_name = "default";
    if (inst->material_name != "default" && ctx.project_mgr) {
        auto resolved = base_path::resolve_asset(inst->material_name, ctx.project_mgr->active());
        auto result = ctx.resource_manager->load_material_asset(resolved, ctx.project_mgr->active());
        if (result) {
            inst->material_handle = *result;
            inst->material = ctx.resource_manager->get(inst->material_handle);
        }
    } else if (inst->material_name == "default") {
        auto tex = ctx.resource_manager->default_white_handle();
        inst->material_handle = ctx.resource_manager->create_material("default", tex);
        inst->material = ctx.resource_manager->get(inst->material_handle);
    }
}

} // namespace

static const bool registered = [] {
    ComponentRegistry::instance().register_component<InstancedMeshComponent>({
        .display_name = "Instanced Mesh",
        .json_key = "instanced_mesh",
        .color = {0.86f, 0.68f, 0.32f, 1.0f},

        .draw = [](Node3D& node, void* ptr, const InspectorContext& ctx) -> bool {
            auto* inst = static_cast<InstancedMeshComponent*>(ptr);
            bool changed = false;
            static const std::string model_exts[] = {".fjmesh"};
            static const std::string material_exts[] = {".fjmat"};

            std::string model_path = node.source;
            if (asset_ref_field({.label = "Model", .id = "##InstMeshModel", .extensions = model_exts, .ctx = &ctx}, model_path)) {
                node.source = model_path;
                resolve_model_for_inst(inst, model_path, ctx);
                changed = true;
            }

            const auto* mat_type = AssetTypeRegistry::instance().find_by_name("Material");
            if (asset_ref_field({.label = "Material", .id = "##InstMeshMaterial", .extensions = material_exts, .ctx = &ctx, .asset_type = mat_type}, inst->material_name)) {
                resolve_material_for_inst(inst, ctx);
                changed = true;
            }

            ImGui::Separator();
            ImGui::Text("Instances: %zu", inst->instances.size());

            // ── Scatter tool ──────────────────────────────────────────
            // Editor state stored in function-local static so it persists
            // across frames; keyed by component ptr so each component owns
            // its own settings.
            struct ScatterState {
                int count{100};
                glm::vec3 box_min{-10.0f, 0.0f, -10.0f};
                glm::vec3 box_max{ 10.0f, 0.0f,  10.0f};
                float scale_min{0.9f};
                float scale_max{1.1f};
                float rotation_jitter{180.0f}; // degrees around Y
                bool snap_to_y_zero{true};
                uint32_t seed{1337};
            };
            static std::unordered_map<InstancedMeshComponent*, ScatterState> scatter_states;
            auto& ss = scatter_states[inst];

            if (ImGui::CollapsingHeader("Scatter", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::DragInt("Count", &ss.count, 1.0f, 1, 100000);
                ImGui::DragFloat3("Box Min", &ss.box_min.x, 0.1f);
                ImGui::DragFloat3("Box Max", &ss.box_max.x, 0.1f);
                ImGui::DragFloatRange2("Scale Range", &ss.scale_min, &ss.scale_max, 0.01f,
                                       0.01f, 100.0f, "%.2f");
                ImGui::DragFloat("Rotation Jitter (deg)", &ss.rotation_jitter, 1.0f, 0.0f, 180.0f);
                ImGui::Checkbox("Snap Y to 0", &ss.snap_to_y_zero);
                int seed_int = static_cast<int>(ss.seed);
                if (ImGui::DragInt("Seed", &seed_int)) ss.seed = static_cast<uint32_t>(seed_int);

                if (ImGui::Button("Scatter (Replace)")) {
                    inst->instances.clear();
                    std::mt19937 rng(ss.seed);
                    std::uniform_real_distribution<float> dx(ss.box_min.x, ss.box_max.x);
                    std::uniform_real_distribution<float> dy(ss.box_min.y, ss.box_max.y);
                    std::uniform_real_distribution<float> dz(ss.box_min.z, ss.box_max.z);
                    std::uniform_real_distribution<float> ds(ss.scale_min, ss.scale_max);
                    float jitter_rad = glm::radians(ss.rotation_jitter);
                    std::uniform_real_distribution<float> dr(-jitter_rad, jitter_rad);
                    inst->instances.reserve(ss.count);
                    for (int i = 0; i < ss.count; ++i) {
                        glm::vec3 pos{dx(rng), ss.snap_to_y_zero ? 0.0f : dy(rng), dz(rng)};
                        float angle = dr(rng);
                        float scale = ds(rng);
                        glm::mat4 m = glm::translate(glm::mat4(1.0f), pos)
                                    * glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0, 1, 0))
                                    * glm::scale(glm::mat4(1.0f), glm::vec3(scale));
                        inst->instances.push_back(m);
                    }
                    inst->dirty = true;
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Scatter (Append)")) {
                    std::mt19937 rng(ss.seed + static_cast<uint32_t>(inst->instances.size()));
                    std::uniform_real_distribution<float> dx(ss.box_min.x, ss.box_max.x);
                    std::uniform_real_distribution<float> dy(ss.box_min.y, ss.box_max.y);
                    std::uniform_real_distribution<float> dz(ss.box_min.z, ss.box_max.z);
                    std::uniform_real_distribution<float> ds(ss.scale_min, ss.scale_max);
                    float jitter_rad = glm::radians(ss.rotation_jitter);
                    std::uniform_real_distribution<float> dr(-jitter_rad, jitter_rad);
                    inst->instances.reserve(inst->instances.size() + ss.count);
                    for (int i = 0; i < ss.count; ++i) {
                        glm::vec3 pos{dx(rng), ss.snap_to_y_zero ? 0.0f : dy(rng), dz(rng)};
                        float angle = dr(rng);
                        float scale = ds(rng);
                        glm::mat4 m = glm::translate(glm::mat4(1.0f), pos)
                                    * glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0, 1, 0))
                                    * glm::scale(glm::mat4(1.0f), glm::vec3(scale));
                        inst->instances.push_back(m);
                    }
                    inst->dirty = true;
                    changed = true;
                }
            }

            // ── Instance list / per-row editing ────────────────────────
            if (ImGui::CollapsingHeader("Instances")) {
                if (ImGui::Button("Add at Origin")) {
                    inst->instances.emplace_back(1.0f);
                    inst->dirty = true;
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear") && !inst->instances.empty()) {
                    inst->instances.clear();
                    inst->dirty = true;
                    changed = true;
                }

                ImGui::BeginChild("##InstanceList", ImVec2(0, 240), true);
                int to_delete = -1;
                for (size_t i = 0; i < inst->instances.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    // Decompose the matrix into translation + scale (Y-only rotation for the UI).
                    glm::mat4& m = inst->instances[i];
                    glm::vec3 pos = glm::vec3(m[3]);
                    glm::vec3 scale{
                        glm::length(glm::vec3(m[0])),
                        glm::length(glm::vec3(m[1])),
                        glm::length(glm::vec3(m[2]))};
                    // Y-axis rotation recovered from the first column after divide by scale.
                    glm::vec3 col0 = glm::vec3(m[0]) / std::max(scale.x, 1e-6f);
                    float yaw_deg = glm::degrees(std::atan2(col0.z, col0.x));

                    char label[32];
                    std::snprintf(label, sizeof(label), "%zu", i);
                    if (ImGui::TreeNode(label, "#%zu  (%+.1f, %+.1f, %+.1f)",
                                        i, pos.x, pos.y, pos.z)) {
                        bool row_changed = false;
                        row_changed |= ImGui::DragFloat3("Position", &pos.x, 0.05f);
                        row_changed |= ImGui::DragFloat("Yaw (deg)", &yaw_deg, 0.5f, -360.0f, 360.0f);
                        row_changed |= ImGui::DragFloat3("Scale", &scale.x, 0.01f, 0.001f, 1000.0f);
                        if (ImGui::Button("Delete")) to_delete = static_cast<int>(i);
                        if (row_changed) {
                            m = glm::translate(glm::mat4(1.0f), pos)
                              * glm::rotate(glm::mat4(1.0f), glm::radians(yaw_deg), glm::vec3(0, 1, 0))
                              * glm::scale(glm::mat4(1.0f), scale);
                            inst->dirty = true;
                            changed = true;
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();

                if (to_delete >= 0) {
                    inst->instances.erase(inst->instances.begin() + to_delete);
                    inst->dirty = true;
                    changed = true;
                }
            }

            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<InstancedMeshComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* proj) {
            auto* inst = static_cast<const InstancedMeshComponent*>(ptr);
            out["material"] = base_path::asset_reference(inst->material_name, proj);

            nlohmann::json arr = nlohmann::json::array();
            arr.get_ptr<nlohmann::json::array_t*>()->reserve(inst->instances.size());
            for (const auto& m : inst->instances) {
                nlohmann::json row = nlohmann::json::array();
                const float* p = glm::value_ptr(m);
                for (int i = 0; i < 16; ++i) row.push_back(p[i]);
                arr.push_back(std::move(row));
            }
            out["instances"] = std::move(arr);
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& res, const Project* proj) {
            if (!node.components.has<InstancedMeshComponent>()) {
                node.components.add<InstancedMeshComponent>();
            }
            auto* inst = node.components.get<InstancedMeshComponent>();
            if (!inst) return;

            inst->material_name = j.value("material", "default");

            if (!node.source.empty()) {
                auto resolved = base_path::resolve_asset(node.source, proj);
                auto result = res.load_model(resolved);
                if (result) {
                    inst->model_handle = *result;
                    inst->model = res.get(inst->model_handle);
                }
            }

            if (inst->material_name != "default" &&
                base_path::references_asset(inst->material_name, ".fjmat")) {
                auto resolved = base_path::resolve_asset(inst->material_name, proj);
                auto result = res.load_material_asset(resolved, proj);
                if (result) {
                    inst->material_handle = *result;
                    inst->material = res.get(inst->material_handle);
                }
            } else {
                auto tex = res.default_white_handle();
                inst->material_handle = res.create_material("default", tex);
                inst->material = res.get(inst->material_handle);
            }

            if (j.contains("instances") && j["instances"].is_array()) {
                inst->instances.clear();
                inst->instances.reserve(j["instances"].size());
                for (const auto& row : j["instances"]) {
                    if (!row.is_array() || row.size() != 16) continue;
                    glm::mat4 m{};
                    float* p = glm::value_ptr(m);
                    for (int i = 0; i < 16; ++i) p[i] = row[i].get<float>();
                    inst->instances.push_back(m);
                }
                inst->dirty = true;
            }
        },

        .script_type_name = "InstancedMeshComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<InstancedMeshComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<InstancedMeshComponent>(); },
    });

    // Register with the scene gatherer so instanced meshes are emitted
    // through the generic instance-provider path. Behavior identical to the
    // previous hardcoded branch in SceneGatherer::gather().
    InstanceProviderRegistry::instance().add(
        [](Node& node, const Node3D& spatial) -> std::optional<DrawCommand> {
            auto* inst = node.components.get<InstancedMeshComponent>();
            if (!inst || !inst->model || inst->instances.empty()) return std::nullopt;

            DrawCommand dc{};
            dc.world_matrix      = spatial.world_matrix_;
            dc.prev_world_matrix = spatial.prev_world_matrix_;
            dc.push.model        = spatial.world_matrix_;
            dc.model             = inst->model;
            dc.node_id           = node.id;
            dc.instance_transforms = &inst->instances;

            if (inst->material) {
                dc.push.material_id = inst->material->material_id();
                dc.pipeline_id      = inst->material->pipeline_id();
                dc.sort_key = RenderQueue::make_sort_key(
                    dc.pipeline_id, inst->material->material_id(), node.id);
                dc.transparent = inst->material->force_transparent()
                              || (inst->material->params().base_color_factor.w < 1.0f);
            }

            // Union AABB cached on the component (local-space) — recompute
            // only when dirty.
            if (inst->dirty) {
                AABB model_local = inst->model->local_aabb();
                bool first = true;
                for (const auto& lxf : inst->instances) {
                    AABB box = model_local.transform(lxf);
                    if (first) {
                        inst->local_bounds = box;
                        first = false;
                    } else {
                        inst->local_bounds.min = glm::min(inst->local_bounds.min, box.min);
                        inst->local_bounds.max = glm::max(inst->local_bounds.max, box.max);
                    }
                }
                inst->dirty = false;
            }
            dc.world_aabb = inst->local_bounds.transform(spatial.world_matrix_);

            return dc;
        });

    return true;
}();

} // namespace fjell
