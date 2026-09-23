#include "scene/component_registry.hpp"
#include "renderer/components/instanced_mesh_component.hpp"
#include "renderer/instance_provider.hpp"
#include "renderer/resources/model.hpp"
#include "scene/node.hpp"
#include "core/base_path.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/material.hpp"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdio>

namespace fjell {


static const bool registered = [] {
    ComponentRegistry::instance().register_component<InstancedMeshComponent>({
        .display_name = "Instanced Mesh",
        .json_key = "instanced_mesh",

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
