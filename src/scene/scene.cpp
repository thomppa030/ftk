#include "scene/scene.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/texture.hpp"
#include "scripting/fjell_actor.hpp"
#include "scripting/fjell_component.hpp"
#include "core/base_path.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace fjell {

// ── Out-of-line special members (need complete Actor/Component types) ───

ScriptEntry::ScriptEntry() = default;
ScriptEntry::~ScriptEntry() = default;
ScriptEntry::ScriptEntry(ScriptEntry&&) noexcept = default;
ScriptEntry& ScriptEntry::operator=(ScriptEntry&&) noexcept = default;

SceneNode::SceneNode() = default;
SceneNode::~SceneNode() = default;
SceneNode::SceneNode(SceneNode&&) noexcept = default;
SceneNode& SceneNode::operator=(SceneNode&&) noexcept = default;

void SceneNode::clone_base_into(SceneNode& target) const {
    target.id = id;
    target.parent_id = parent_id;
    target.world_matrix_ = world_matrix_;
    target.transform = transform;
    target.name = name;
    target.source = source;
    target.mesh = mesh;
    target.actor_type_name = actor_type_name;
    for (const auto& s : scripts) {
        ScriptEntry entry;
        entry.type_name = s.type_name;
        entry.serialized = s.serialized;
        target.scripts.push_back(std::move(entry));
    }
    target.rigidbody = rigidbody;
    target.collider = collider;
    target.emitter = emitter;
    target.sprite = sprite;
    target.arrow = arrow;
    if (audio) {
        target.audio = AudioSourceComponent{};
        target.audio->clip_path = audio->clip_path;
        target.audio->volume = audio->volume;
        target.audio->pitch = audio->pitch;
        target.audio->min_distance = audio->min_distance;
        target.audio->max_distance = audio->max_distance;
        target.audio->loop = audio->loop;
        target.audio->spatial = audio->spatial;
        target.audio->play_on_start = audio->play_on_start;
    }
}

std::unique_ptr<SceneNode> SceneNode::clone() const {
    auto copy = std::make_unique<SceneNode>();
    clone_base_into(*copy);
    return copy;
}

uint32_t Scene::add(ModelHandle model_handle, MaterialHandle material_handle,
                    ResourceManager& resources, Transform transform,
                    const std::string& name, const std::string& source,
                    const std::string& material_name) {
    uint32_t id = next_id_++;
    auto obj = std::make_unique<SceneNode>();
    obj->id = id;
    obj->mesh = MeshComponent{};
    obj->mesh->model_handle = model_handle;
    obj->mesh->material_handle = material_handle;
    obj->mesh->model = resources.get(model_handle);
    obj->mesh->material = resources.get(material_handle);
    obj->mesh->material_name = material_name;
    obj->transform = transform;
    obj->name = name;
    obj->source = source;
    objects_.push_back(std::move(obj));
    dirty_ = true;
    return id;
}

uint32_t Scene::add_empty(Transform transform, const std::string& name) {
    uint32_t id = next_id_++;
    auto obj = std::make_unique<SceneNode>();
    obj->id = id;
    obj->transform = transform;
    obj->name = name;
    objects_.push_back(std::move(obj));
    dirty_ = true;
    return id;
}

uint32_t Scene::add_node(std::unique_ptr<SceneNode> node) {
    if (node->id == 0) node->id = next_id_++;
    if (node->id >= next_id_) next_id_ = node->id + 1;
    uint32_t id = node->id;
    objects_.push_back(std::move(node));
    dirty_ = true;
    return id;
}

void Scene::remove(uint32_t id) {
    // Orphan children first
    for (auto& obj : objects_) {
        if (obj->parent_id == static_cast<int32_t>(id)) {
            obj->parent_id = -1;
        }
    }
    auto it = std::find_if(objects_.begin(), objects_.end(),
                           [id](const auto& o) { return o->id == id; });
    if (it != objects_.end()) {
        objects_.erase(it);
        dirty_ = true;
    }
}

std::unique_ptr<SceneNode> Scene::extract(uint32_t id) {
    // Orphan children
    for (auto& obj : objects_) {
        if (obj->parent_id == static_cast<int32_t>(id)) {
            obj->parent_id = -1;
        }
    }
    auto it = std::find_if(objects_.begin(), objects_.end(),
                           [id](const auto& o) { return o->id == id; });
    auto result = std::move(*it);
    objects_.erase(it);
    dirty_ = true;
    return result;
}

void Scene::restore(std::unique_ptr<SceneNode> node) {
    if (node->id >= next_id_) next_id_ = node->id + 1;
    objects_.push_back(std::move(node));
    dirty_ = true;
}

void Scene::insert_point_light(int index, const PointLight& light) {
    point_lights_.insert(point_lights_.begin() + index, light);
    dirty_ = true;
}

void Scene::insert_spot_light(int index, const SpotLight& light) {
    spot_lights_.insert(spot_lights_.begin() + index, light);
    dirty_ = true;
}

void Scene::remove_point_light(int index) {
    point_lights_.erase(point_lights_.begin() + index);
    dirty_ = true;
}

void Scene::remove_spot_light(int index) {
    spot_lights_.erase(spot_lights_.begin() + index);
    dirty_ = true;
}

void Scene::clear() {
    objects_.clear();
    point_lights_.clear();
    spot_lights_.clear();
}

void Scene::rebuild_id_index() {
    id_to_index_.clear();
    id_to_index_.reserve(objects_.size());
    for (int i = 0; i < static_cast<int>(objects_.size()); ++i) {
        id_to_index_[objects_[i]->id] = i;
    }
}

SceneNode* Scene::find(uint32_t id) {
    auto it = id_to_index_.find(id);
    if (it != id_to_index_.end() && it->second < static_cast<int>(objects_.size())) {
        return objects_[it->second].get();
    }
    // Fallback: linear scan (index may be stale after remove)
    for (auto& obj : objects_) {
        if (obj->id == id) return obj.get();
    }
    return nullptr;
}

const SceneNode* Scene::find(uint32_t id) const {
    auto it = id_to_index_.find(id);
    if (it != id_to_index_.end() && it->second < static_cast<int>(objects_.size())) {
        return objects_[it->second].get();
    }
    for (const auto& obj : objects_) {
        if (obj->id == id) return obj.get();
    }
    return nullptr;
}

int Scene::index_of(uint32_t id) const {
    auto it = id_to_index_.find(id);
    if (it != id_to_index_.end()) return it->second;
    return -1;
}

void Scene::compute_world_matrices() {
    // Rebuild O(1) id→index lookup
    rebuild_id_index();

    // Single-pass topological traversal: process each node once its parent is done.
    // Most scenes are shallow (depth 2-4), so this converges in 2-3 passes max.
    std::vector<bool> computed(objects_.size(), false);
    auto remaining = objects_.size();

    // Pass 1: roots (no parent)
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        if (objects_[i]->parent_id == -1) {
            objects_[i]->world_matrix_ = objects_[i]->transform.matrix();
            computed[i] = true;
            --remaining;
        }
    }

    // Subsequent passes: children whose parent is already computed (O(1) lookup)
    while (remaining > 0) {
        auto prev = remaining;
        for (std::size_t i = 0; i < objects_.size(); ++i) {
            if (computed[i]) continue;
            int pi = index_of(static_cast<uint32_t>(objects_[i]->parent_id));
            if (pi < 0) {
                objects_[i]->parent_id = -1;
                objects_[i]->world_matrix_ = objects_[i]->transform.matrix();
                computed[i] = true;
                --remaining;
            } else if (computed[pi]) {
                objects_[i]->world_matrix_ =
                    objects_[pi]->world_matrix_ * objects_[i]->transform.matrix();
                computed[i] = true;
                --remaining;
            }
        }
        if (remaining == prev) {
            for (std::size_t i = 0; i < objects_.size(); ++i) {
                if (!computed[i]) {
                    objects_[i]->parent_id = -1;
                    objects_[i]->world_matrix_ = objects_[i]->transform.matrix();
                }
            }
            break;
        }
    }
}

void Scene::reparent(uint32_t child_id, int32_t new_parent_id) {
    auto* child = find(child_id);
    if (!child) return;

    glm::mat4 child_world = child->world_matrix_;
    child->parent_id = new_parent_id;

    // Adjust local transform to preserve world position
    glm::mat4 new_local = child_world;
    if (new_parent_id != -1) {
        const auto* parent = find(static_cast<uint32_t>(new_parent_id));
        if (parent) {
            new_local = glm::inverse(parent->world_matrix_) * child_world;
        }
    }
    child->transform = decompose_matrix(new_local);
    dirty_ = true;
}

void Scene::save(const std::string& path) {
    nlohmann::json j;
    j["next_id"] = next_id_;
    j["objects"] = nlohmann::json::array();

    for (const auto& obj : objects_) {
        nlohmann::json jobj;
        jobj["id"] = obj->id;
        jobj["parent_id"] = obj->parent_id;
        jobj["name"] = obj->name;
        jobj["source"] = obj->source;
        if (obj->mesh) {
            nlohmann::json jmesh;
            jmesh["material"] = obj->mesh->material_name;
            if (obj->mesh->material) {
                const auto& bcf = obj->mesh->material->params().base_color_factor;
                if (bcf.r != 1.0f || bcf.g != 1.0f || bcf.b != 1.0f || bcf.a != 1.0f) {
                    if (bcf.a < 1.0f) {
                        jmesh["color"] = {bcf.r, bcf.g, bcf.b, bcf.a};
                    } else {
                        jmesh["color"] = {bcf.r, bcf.g, bcf.b};
                    }
                }
            }
            jobj["mesh"] = jmesh;
        }
        jobj["position"] = {obj->transform.position.x, obj->transform.position.y, obj->transform.position.z};
        {
            auto euler = obj->transform.euler_degrees();
            jobj["rotation"] = {euler.x, euler.y, euler.z};
        }
        jobj["scale"] = {obj->transform.scale.x, obj->transform.scale.y, obj->transform.scale.z};

        if (obj->rigidbody) {
            auto& rb = *obj->rigidbody;
            jobj["rigidbody"] = {
                {"body_type", static_cast<int>(rb.body_type)},
                {"mass", rb.mass},
                {"friction", rb.friction},
                {"restitution", rb.restitution},
                {"linear_damping", rb.linear_damping},
                {"angular_damping", rb.angular_damping},
                {"gravity_enabled", rb.gravity_enabled}
            };
        }
        if (obj->collider) {
            auto& col = *obj->collider;
            jobj["collider"] = {
                {"shape", static_cast<int>(col.shape)},
                {"half_extents", {col.half_extents.x, col.half_extents.y, col.half_extents.z}},
                {"radius", col.radius},
                {"height", col.height},
                {"offset", {col.offset.x, col.offset.y, col.offset.z}}
            };
        }

        if (obj->emitter) {
            auto& em = *obj->emitter;
            jobj["emitter"] = {
                {"emission_rate", em.emission_rate},
                {"lifetime_min", em.lifetime_min},
                {"lifetime_max", em.lifetime_max},
                {"max_particles", em.max_particles},
                {"velocity_min", {em.velocity_min.x, em.velocity_min.y, em.velocity_min.z}},
                {"velocity_max", {em.velocity_max.x, em.velocity_max.y, em.velocity_max.z}},
                {"gravity", {em.gravity.x, em.gravity.y, em.gravity.z}},
                {"color_start", {em.color_start.r, em.color_start.g, em.color_start.b, em.color_start.a}},
                {"color_end", {em.color_end.r, em.color_end.g, em.color_end.b, em.color_end.a}},
                {"size_start", em.size_start},
                {"size_end", em.size_end},
                {"blend_mode", static_cast<int>(em.blend_mode)}
            };
        }

        if (obj->sprite) {
            auto& sp = *obj->sprite;
            jobj["sprite"] = {
                {"texture", sp.texture_path},
                {"tint", {sp.tint.r, sp.tint.g, sp.tint.b, sp.tint.a}},
                {"opacity", sp.opacity},
                {"editor_only", sp.editor_only}
            };
        }

        if (obj->audio) {
            auto& au = *obj->audio;
            jobj["audio"] = {
                {"clip", au.clip_path},
                {"volume", au.volume},
                {"pitch", au.pitch},
                {"min_distance", au.min_distance},
                {"max_distance", au.max_distance},
                {"loop", au.loop},
                {"spatial", au.spatial},
                {"play_on_start", au.play_on_start}
            };
        }

        if (obj->arrow) {
            auto& arr = *obj->arrow;
            jobj["arrow"] = {
                {"length", arr.length},
                {"color", {arr.color.x, arr.color.y, arr.color.z}}
            };
        }

        if (!obj->scripts.empty()) {
            jobj["components"] = nlohmann::json::array();
            for (const auto& comp : obj->scripts) {
                nlohmann::json jcomp;
                jcomp["type"] = comp.type_name;
                jcomp["data"] = comp.serialized;
                jobj["components"].push_back(jcomp);
            }
        }

        // Typed node data
        if (auto* cam = dynamic_cast<const CameraNode*>(obj.get())) {
            jobj["node_type"] = "camera";
            jobj["camera"] = {
                {"fov", cam->fov},
                {"near", cam->near_plane},
                {"far", cam->far_plane}
            };
        } else if (auto* dl = dynamic_cast<const DirectionalLightNode*>(obj.get())) {
            jobj["node_type"] = "directional_light";
            jobj["light"] = {
                {"direction", {dl->direction.x, dl->direction.y, dl->direction.z}},
                {"color", {dl->color.x, dl->color.y, dl->color.z}},
                {"intensity", dl->intensity},
                {"ambient_color", {dl->ambient_color.x, dl->ambient_color.y, dl->ambient_color.z}},
                {"ambient_intensity", dl->ambient_intensity}
            };
        } else if (auto* pl = dynamic_cast<const PointLightNode*>(obj.get())) {
            jobj["node_type"] = "point_light";
            jobj["light"] = {
                {"color", {pl->color.x, pl->color.y, pl->color.z}},
                {"intensity", pl->intensity},
                {"radius", pl->radius}
            };
        } else if (auto* sl = dynamic_cast<const SpotLightNode*>(obj.get())) {
            jobj["node_type"] = "spot_light";
            jobj["light"] = {
                {"direction", {sl->direction.x, sl->direction.y, sl->direction.z}},
                {"color", {sl->color.x, sl->color.y, sl->color.z}},
                {"intensity", sl->intensity},
                {"radius", sl->radius},
                {"inner_angle", sl->inner_angle},
                {"outer_angle", sl->outer_angle}
            };
        }

        j["objects"].push_back(jobj);
    }

    // Point lights
    if (!point_lights_.empty()) {
        j["point_lights"] = nlohmann::json::array();
        for (const auto& pl : point_lights_) {
            j["point_lights"].push_back({
                {"position", {pl.position.x, pl.position.y, pl.position.z}},
                {"color", {pl.color.x, pl.color.y, pl.color.z}},
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
                {"color", {sl.color.x, sl.color.y, sl.color.z}},
                {"intensity", sl.intensity},
                {"radius", sl.radius},
                {"inner_angle", sl.inner_angle},
                {"outer_angle", sl.outer_angle}
            });
        }
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        FJELL_CORE_ERROR("Failed to save scene: {}", path);
        return;
    }
    file << j.dump(2);
    dirty_ = false;
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
    auto default_tex = resources.load_texture(
        base_path::resolve_asset("assets/textures/test.png", project));

    uint32_t max_id = 0;

    for (const auto& jobj : j["objects"]) {
        std::string name = jobj.value("name", "Object");
        std::string source = jobj.value("source", "");

        // Read mesh fields from "mesh" key or fall back to top-level (backward compat)
        std::string mat_name = "default";
        nlohmann::json color_json;
        if (jobj.contains("mesh")) {
            const auto& jmesh = jobj["mesh"];
            mat_name = jmesh.value("material", "default");
            if (jmesh.contains("color")) color_json = jmesh["color"];
        } else {
            mat_name = jobj.value("material", "default");
            if (jobj.contains("color")) color_json = jobj["color"];
        }

        Transform transform;
        if (jobj.contains("position")) {
            auto& p = jobj["position"];
            transform.position = {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
        }
        if (jobj.contains("rotation")) {
            auto& r = jobj["rotation"];
            transform.set_euler_degrees({r[0].get<float>(), r[1].get<float>(), r[2].get<float>()});
        }
        if (jobj.contains("scale")) {
            auto& s = jobj["scale"];
            transform.scale = {s[0].get<float>(), s[1].get<float>(), s[2].get<float>()};
        }

        // Light nodes
        std::string node_type = jobj.value("node_type", "");
        if (!node_type.empty()) {
            std::unique_ptr<SceneNode> node;
            if (node_type == "camera") {
                auto cam = std::make_unique<CameraNode>();
                if (jobj.contains("camera")) {
                    auto& jc = jobj["camera"];
                    cam->fov = jc.value("fov", 45.0f);
                    cam->near_plane = jc.value("near", 0.1f);
                    cam->far_plane = jc.value("far", 1000.0f);
                }
                node = std::move(cam);
            } else if (node_type == "directional_light") {
                auto dl = std::make_unique<DirectionalLightNode>();
                if (jobj.contains("light")) {
                    auto& jl = jobj["light"];
                    if (jl.contains("direction")) {
                        auto& d = jl["direction"];
                        dl->direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()};
                    }
                    if (jl.contains("color")) {
                        auto& c = jl["color"];
                        dl->color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                    }
                    dl->intensity = jl.value("intensity", 1.5f);
                    if (jl.contains("ambient_color")) {
                        auto& c = jl["ambient_color"];
                        dl->ambient_color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                    }
                    dl->ambient_intensity = jl.value("ambient_intensity", 1.0f);
                }
                node = std::move(dl);
            } else if (node_type == "point_light") {
                auto pl = std::make_unique<PointLightNode>();
                if (jobj.contains("light")) {
                    auto& jl = jobj["light"];
                    if (jl.contains("color")) {
                        auto& c = jl["color"];
                        pl->color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                    }
                    pl->intensity = jl.value("intensity", 1.0f);
                    pl->radius = jl.value("radius", 10.0f);
                }
                node = std::move(pl);
            } else if (node_type == "spot_light") {
                auto sl = std::make_unique<SpotLightNode>();
                if (jobj.contains("light")) {
                    auto& jl = jobj["light"];
                    if (jl.contains("direction")) {
                        auto& d = jl["direction"];
                        sl->direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()};
                    }
                    if (jl.contains("color")) {
                        auto& c = jl["color"];
                        sl->color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                    }
                    sl->intensity = jl.value("intensity", 1.0f);
                    sl->radius = jl.value("radius", 10.0f);
                    sl->inner_angle = jl.value("inner_angle", 20.0f);
                    sl->outer_angle = jl.value("outer_angle", 30.0f);
                }
                node = std::move(sl);
            }

            if (node) {
                node->name = name;
                node->transform = transform;
                uint32_t loaded_id = jobj.value("id", 0u);
                int32_t loaded_parent = jobj.value("parent_id", -1);
                if (loaded_id > 0) {
                    node->id = loaded_id;
                    if (loaded_id > max_id) max_id = loaded_id;
                }
                node->parent_id = loaded_parent;
                scene.add_node(std::move(node));
                continue;
            }
        }

        // Empty nodes have no source and no mesh
        bool has_mesh = !source.empty() || jobj.contains("mesh") || jobj.contains("material");
        uint32_t id = 0;

        if (has_mesh) {
            ModelHandle model_handle;
            if (source.starts_with("primitive:")) {
                std::string type = source.substr(10);
                model_handle = resources.create_primitive(type);
            } else if (!source.empty()) {
                model_handle = resources.load_model(base_path::resolve_asset(source, project));
            }

            if (!model_handle.valid()) continue;

            MaterialHandle mat_handle;
            if (mat_name.ends_with(".fjmat")) {
                std::string mat_path = base_path::resolve_asset(mat_name, project);
                mat_handle = resources.load_material_asset(mat_path, project);
            } else if (!color_json.is_null()) {
                auto& c = color_json;
                PbrParams params{};
                float alpha = c.size() > 3 ? c[3].get<float>() : 1.0f;
                params.base_color_factor = {c[0].get<float>(), c[1].get<float>(),
                                            c[2].get<float>(), alpha};
                std::string color_mat_name = mat_name + "_color_" + std::to_string(jobj.value("id", 0));
                mat_handle = resources.create_material(color_mat_name,
                    resources.default_white_handle(), {}, {}, {}, params);
            } else {
                mat_handle = resources.create_material(mat_name, default_tex);
            }

            auto* model = resources.get(model_handle);
            if (!mat_name.ends_with(".fjmat") &&
                !source.starts_with("primitive:") && model && !model->textures().empty()) {
                mat_handle = resources.create_material_from_texture(
                    source + "_mat",
                    const_cast<Texture*>(model->textures()[0].get()));
            }

            id = scene.add(model_handle, mat_handle, resources,
                           transform, name, source, mat_name);
        } else {
            id = scene.add_empty(transform, name);
        }

        // Override with loaded ID if present (backward compat: use assigned ID)
        uint32_t loaded_id = jobj.value("id", 0u);
        int32_t loaded_parent = jobj.value("parent_id", -1);
        auto* node = scene.find(id);
        if (loaded_id > 0 && node) {
            node->id = loaded_id;
            if (loaded_id > max_id) max_id = loaded_id;
        } else {
            if (id > max_id) max_id = id;
        }
        if (node) node->parent_id = loaded_parent;

        // Load rigidbody descriptor
        if (jobj.contains("rigidbody") && node) {
            auto& jrb = jobj["rigidbody"];
            RigidbodyComponent rb;
            rb.body_type = static_cast<BodyType>(jrb.value("body_type", 2));
            rb.mass = jrb.value("mass", 1.0f);
            rb.friction = jrb.value("friction", 0.5f);
            rb.restitution = jrb.value("restitution", 0.3f);
            rb.linear_damping = jrb.value("linear_damping", 0.05f);
            rb.angular_damping = jrb.value("angular_damping", 0.05f);
            rb.gravity_enabled = jrb.value("gravity_enabled", true);
            node->rigidbody = rb;
        }
        // Load collider descriptor
        if (jobj.contains("collider") && node) {
            auto& jcol = jobj["collider"];
            ColliderComponent col;
            col.shape = static_cast<ColliderShape>(jcol.value("shape", 0));
            if (jcol.contains("half_extents")) {
                auto& he = jcol["half_extents"];
                col.half_extents = {he[0].get<float>(), he[1].get<float>(), he[2].get<float>()};
            }
            col.radius = jcol.value("radius", 0.5f);
            col.height = jcol.value("height", 1.0f);
            if (jcol.contains("offset")) {
                auto& o = jcol["offset"];
                col.offset = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()};
            }
            node->collider = col;
        }

        // Load particle emitter
        if (jobj.contains("emitter") && node) {
            auto& jem = jobj["emitter"];
            EmitterComponent em;
            em.emission_rate = jem.value("emission_rate", 50.0f);
            em.lifetime_min = jem.value("lifetime_min", 1.0f);
            em.lifetime_max = jem.value("lifetime_max", 3.0f);
            em.max_particles = jem.value("max_particles", 10000u);
            if (jem.contains("velocity_min")) {
                auto& v = jem["velocity_min"];
                em.velocity_min = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
            }
            if (jem.contains("velocity_max")) {
                auto& v = jem["velocity_max"];
                em.velocity_max = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
            }
            if (jem.contains("gravity")) {
                auto& g = jem["gravity"];
                em.gravity = {g[0].get<float>(), g[1].get<float>(), g[2].get<float>()};
            }
            if (jem.contains("color_start")) {
                auto& c = jem["color_start"];
                em.color_start = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
            }
            if (jem.contains("color_end")) {
                auto& c = jem["color_end"];
                em.color_end = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
            }
            em.size_start = jem.value("size_start", 0.1f);
            em.size_end = jem.value("size_end", 0.02f);
            em.blend_mode = static_cast<ParticleBlendMode>(jem.value("blend_mode", 0));
            node->emitter = em;
        }

        // Load sprite
        if (jobj.contains("sprite") && node) {
            auto& jsp = jobj["sprite"];
            SpriteComponent sp;
            sp.texture_path = jsp.value("texture", "");
            if (jsp.contains("tint")) {
                auto& t = jsp["tint"];
                sp.tint = {t[0].get<float>(), t[1].get<float>(), t[2].get<float>(), t[3].get<float>()};
            }
            sp.opacity = jsp.value("opacity", 1.0f);
            sp.editor_only = jsp.value("editor_only", false);
            node->sprite = sp;
        }

        // Load audio source
        if (jobj.contains("audio") && node) {
            auto& jau = jobj["audio"];
            AudioSourceComponent au;
            au.clip_path = jau.value("clip", "");
            au.volume = jau.value("volume", 1.0f);
            au.pitch = jau.value("pitch", 1.0f);
            au.min_distance = jau.value("min_distance", 1.0f);
            au.max_distance = jau.value("max_distance", 50.0f);
            au.loop = jau.value("loop", false);
            au.spatial = jau.value("spatial", true);
            au.play_on_start = jau.value("play_on_start", false);
            node->audio = au;
        }

        // Load arrow
        if (jobj.contains("arrow") && node) {
            auto& jar = jobj["arrow"];
            ArrowComponent arr;
            arr.length = jar.value("length", 2.0f);
            if (jar.contains("color")) {
                auto& c = jar["color"];
                arr.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
            }
            node->arrow = arr;
        }

        // Load component data as raw JSON (ScriptManager will instantiate later)
        if (jobj.contains("components") && node) {
            for (const auto& jcomp : jobj["components"]) {
                ScriptEntry comp;
                comp.type_name = jcomp.value("type", "");
                if (jcomp.contains("data")) {
                    comp.serialized = jcomp["data"];
                }
                node->scripts.push_back(std::move(comp));
            }
        }
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
                pl.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
            }
            pl.intensity = jpl.value("intensity", 1.0f);
            pl.radius = jpl.value("radius", 10.0f);
            scene.point_lights_.push_back(pl);
        }
    }

    // Load spot lights
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
                sl.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
            }
            sl.intensity = jsl.value("intensity", 1.0f);
            sl.radius = jsl.value("radius", 10.0f);
            sl.inner_angle = jsl.value("inner_angle", 20.0f);
            sl.outer_angle = jsl.value("outer_angle", 30.0f);
            scene.spot_lights_.push_back(sl);
        }
    }

    // Restore next_id from file or compute from max
    if (j.contains("next_id")) {
        scene.set_next_id(j["next_id"].get<uint32_t>());
    } else {
        scene.set_next_id(max_id + 1);
    }

    FJELL_CORE_INFO("Scene loaded: {} objects from {}", scene.object_count(), path);
    return scene;
}

} // namespace fjell
