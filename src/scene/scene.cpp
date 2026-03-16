#include "scene/scene.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/texture.hpp"
#include "renderer/uniform_types.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace fjell {

uint32_t Scene::add(Model* model, Material* material, Transform transform,
                    const std::string& name, const std::string& source,
                    const std::string& material_name) {
    objects_.push_back({model, material, transform, name, source, material_name});
    return static_cast<uint32_t>(objects_.size() - 1);
}

void Scene::remove(uint32_t id) {
    if (id < objects_.size()) {
        objects_.erase(objects_.begin() + id);
    }
}

void Scene::clear() {
    objects_.clear();
}

void Scene::render(VkCommandBuffer cmd, VkPipelineLayout pipeline_layout,
                   RenderStats* stats) const {
    for (const auto& obj : objects_) {
        PbrPushConstants push{};
        push.model = obj.transform.matrix();
        if (obj.material) {
            const auto& params = obj.material->params();
            push.base_color_factor = params.base_color_factor;
            push.metallic_factor = params.metallic_factor;
            push.roughness_factor = params.roughness_factor;
            push.has_normal_map = params.has_normal_map ? 1.0f : 0.0f;
        }
        vkCmdPushConstants(cmd, pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PbrPushConstants), &push);

        if (obj.material) {
            obj.material->bind(cmd, pipeline_layout);
        }

        if (stats) {
            stats->draw_calls += obj.model->mesh_count();
            stats->triangle_count += obj.model->total_indices() / 3;
        }

        obj.model->draw(cmd);
    }
}

void Scene::save(const std::string& path) const {
    nlohmann::json j;
    j["objects"] = nlohmann::json::array();

    for (const auto& obj : objects_) {
        nlohmann::json jobj;
        jobj["name"] = obj.name;
        jobj["source"] = obj.source;
        jobj["material"] = obj.material_name;
        jobj["position"] = {obj.transform.position.x, obj.transform.position.y, obj.transform.position.z};
        jobj["rotation"] = {obj.transform.rotation.x, obj.transform.rotation.y, obj.transform.rotation.z};
        jobj["scale"] = {obj.transform.scale.x, obj.transform.scale.y, obj.transform.scale.z};
        j["objects"].push_back(jobj);
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        FJELL_CORE_ERROR("Failed to save scene: {}", path);
        return;
    }
    file << j.dump(2);
    FJELL_CORE_INFO("Scene saved: {}", path);
}

Scene Scene::load(const std::string& path, ResourceManager& resources,
                  const Project* project) {
    std::ifstream file(path);
    if (!file.is_open()) {
        FJELL_CORE_WARN("Scene file not found: {}", path);
        return {};
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        FJELL_CORE_ERROR("Failed to parse scene: {}", e.what());
        return {};
    }

    Scene scene;
    auto* default_tex = resources.load_texture(
        base_path::resolve_asset("assets/textures/test.png", project));

    for (const auto& jobj : j["objects"]) {
        std::string name = jobj.value("name", "Object");
        std::string source = jobj.value("source", "");
        std::string mat_name = jobj.value("material", "default");

        Model* model = nullptr;
        if (source.starts_with("primitive:")) {
            std::string type = source.substr(10);
            model = resources.create_primitive(type);
        } else if (!source.empty()) {
            model = resources.load_model(base_path::resolve_asset(source, project));
        }

        if (!model) continue;

        auto* mat = resources.create_material(mat_name, default_tex);

        // If it's a file-loaded model with embedded textures, use those
        if (!source.starts_with("primitive:") && !model->textures().empty()) {
            mat = resources.create_material(
                source + "_mat",
                const_cast<Texture*>(model->textures()[0].get()));
        }

        Transform transform;
        if (jobj.contains("position")) {
            auto& p = jobj["position"];
            transform.position = {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
        }
        if (jobj.contains("rotation")) {
            auto& r = jobj["rotation"];
            transform.rotation = {r[0].get<float>(), r[1].get<float>(), r[2].get<float>()};
        }
        if (jobj.contains("scale")) {
            auto& s = jobj["scale"];
            transform.scale = {s[0].get<float>(), s[1].get<float>(), s[2].get<float>()};
        }

        scene.add(model, mat, transform, name, source, mat_name);
    }

    FJELL_CORE_INFO("Scene loaded: {} objects from {}", scene.object_count(), path);
    return scene;
}

} // namespace fjell
