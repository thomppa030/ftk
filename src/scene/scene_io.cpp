#include "scene/scene.hpp"
#include "scene/node_serializer.hpp"
#include "core/log.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/resources/resource_manager.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace fjell {

// The scene's ties to the asset system: creating a node from resource
// handles, and reading or writing the scene file. Kept apart from the graph
// container so that can be used without a ResourceManager behind it.

uint32_t Scene::add(ModelHandle model_handle, MaterialHandle material_handle,
                    ResourceManager& resources, Transform transform,
                    const std::string& name, const std::string& source,
                    const std::string& material_name) {
    uint32_t id = next_id_++;
    auto obj = std::make_unique<Node3D>();
    obj->id = id;
    auto& mc = obj->components.add<MeshComponent>();
    mc.model_handle = model_handle;
    mc.material_handle = material_handle;
    mc.model = resources.get(model_handle);
    mc.material = resources.get(material_handle);
    mc.material_name = material_name;
    obj->transform = transform;
    obj->name = name;
    obj->source = source;
    return add_node(std::move(obj));
}

nlohmann::json Scene::to_json(const Project* project) const {
    nlohmann::json j;
    j["next_id"] = next_id_;
    j["objects"] = nlohmann::json::array();

    for (const auto& obj : objects_) {
        j["objects"].push_back(serialize_node(*obj, project));
    }

    // Point lights
    if (!point_lights_.empty()) {
        j["point_lights"] = nlohmann::json::array();
        for (const auto& pl : point_lights_) {
            j["point_lights"].push_back({
                {"position", {pl.position.x, pl.position.y, pl.position.z}},
                {"color", color_to_json(pl.color)},
                {"intensity", pl.intensity},
                {"radius", pl.radius}
            });
        }
    }

    // Spot lights
    if (!spot_lights_.empty()) {
        j["spot_lights"] = nlohmann::json::array();
        for (const auto& sl : spot_lights_) {
            j["spot_lights"].push_back({
                {"position", {sl.position.x, sl.position.y, sl.position.z}},
                {"direction", {sl.direction.x, sl.direction.y, sl.direction.z}},
                {"color", color_to_json(sl.color)},
                {"intensity", sl.intensity},
                {"radius", sl.radius},
                {"inner_angle", sl.inner_angle},
                {"outer_angle", sl.outer_angle}
            });
        }
    }

    return j;
}

Result<> Scene::save(const std::string& path, const Project* project) {
    std::ofstream file(path);
    if (!file.is_open()) {
        return make_error("Failed to save scene: " + path);
    }
    file << to_json(project).dump(2);
    dirty_ = false;
    FJELL_CORE_INFO("Scene saved: {}", path);
    return {};
}

Result<Scene> Scene::load(const std::string& path, ResourceManager& resources,
                          const Project* project) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return make_error("Scene file not found: " + path);
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(std::string("Failed to parse scene: ") + e.what());
    }

    Scene scene = from_json(j, resources, project);
    FJELL_CORE_INFO("Scene loaded: {} objects from {}", scene.object_count(), path);
    return scene;
}

Scene Scene::from_json(const nlohmann::json& j, ResourceManager& resources,
                       const Project* project) {
    Scene scene;
    uint32_t max_id = 0;

    for (const auto& jobj : j["objects"]) {
        auto node = deserialize_node(jobj, resources, project);
        if (!node) {
            FJELL_CORE_ERROR("Scene: node '{}' could not be deserialized and was dropped",
                             jobj.value("name", "?"));
            continue;
        }

        if (node->id > 0 && node->id > max_id) {
            max_id = node->id;
        }
        scene.add_node(std::move(node));
    }

    // Load point lights
    if (j.contains("point_lights")) {
        for (const auto& jpl : j["point_lights"]) {
            PointLight pl;
            if (jpl.contains("position")) {
                auto& p = jpl["position"];
                pl.position = {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
            }
            if (jpl.contains("color")) {
                auto& c = jpl["color"];
                pl.color = color_from_json(c);
            }
            pl.intensity = jpl.value("intensity", 1.0f);
            pl.radius = jpl.value("radius", 10.0f);
            scene.point_lights_.push_back(pl);
        }
    }

    if (j.contains("spot_lights")) {
        for (const auto& jsl : j["spot_lights"]) {
            SpotLight sl;
            if (jsl.contains("position")) {
                auto& p = jsl["position"];
                sl.position = {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
            }
            if (jsl.contains("direction")) {
                auto& d = jsl["direction"];
                sl.direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()};
            }
            if (jsl.contains("color")) {
                auto& c = jsl["color"];
                sl.color = color_from_json(c);
            }
            sl.intensity = jsl.value("intensity", 1.0f);
            sl.radius = jsl.value("radius", 10.0f);
            sl.inner_angle = jsl.value("inner_angle", 20.0f);
            sl.outer_angle = jsl.value("outer_angle", 30.0f);
            scene.spot_lights_.push_back(sl);
        }
    }

    if (j.contains("next_id")) {
        scene.set_next_id(j["next_id"].get<uint32_t>());
    } else {
        scene.set_next_id(max_id + 1);
    }

    return scene;
}

} // namespace fjell
