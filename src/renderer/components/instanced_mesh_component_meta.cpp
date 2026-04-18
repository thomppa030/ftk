#include "scene/component_registry.hpp"
#include "renderer/components/instanced_mesh_component.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"
#include "core/base_path.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/material.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <glm/gtc/type_ptr.hpp>

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
            if (ImGui::Button("Add Instance at Origin")) {
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

            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<InstancedMeshComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* inst = static_cast<const InstancedMeshComponent*>(ptr);
            out["material"] = inst->material_name;

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

            if (inst->material_name != "default" && inst->material_name.ends_with(".fjmat")) {
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
    return true;
}();

} // namespace fjell
