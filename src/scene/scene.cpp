#include "scene/scene.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/texture.hpp"
#include "scripting/fjell_actor.hpp"
#include "scripting/fjell_component.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace fjell {

// ── Out-of-line special members (need complete Actor/Component types) ───

ComponentEntry::ComponentEntry() = default;
ComponentEntry::~ComponentEntry() = default;
ComponentEntry::ComponentEntry(ComponentEntry&&) noexcept = default;
ComponentEntry& ComponentEntry::operator=(ComponentEntry&&) noexcept = default;

RenderObject::RenderObject() = default;
RenderObject::~RenderObject() = default;
RenderObject::RenderObject(RenderObject&&) noexcept = default;
RenderObject& RenderObject::operator=(RenderObject&&) noexcept = default;

uint32_t Scene::add(ModelHandle model_handle, MaterialHandle material_handle,
                    ResourceManager& resources, Transform transform,
                    const std::string& name, const std::string& source,
                    const std::string& material_name) {
    uint32_t id = next_id_++;
    RenderObject obj{};
    obj.id = id;
    obj.model_handle = model_handle;
    obj.material_handle = material_handle;
    obj.model = resources.get(model_handle);
    obj.material = resources.get(material_handle);
    obj.transform = transform;
    obj.name = name;
    obj.source = source;
    obj.material_name = material_name;
    objects_.push_back(std::move(obj));
    return id;
}

void Scene::remove(uint32_t id) {
    // Orphan children first
    for (auto& obj : objects_) {
        if (obj.parent_id == static_cast<int32_t>(id)) {
            obj.parent_id = -1;
        }
    }
    auto it = std::find_if(objects_.begin(), objects_.end(),
                           [id](const RenderObject& o) { return o.id == id; });
    if (it != objects_.end()) {
        objects_.erase(it);
    }
}

void Scene::clear() {
    objects_.clear();
}

RenderObject* Scene::find(uint32_t id) {
    for (auto& obj : objects_) {
        if (obj.id == id) return &obj;
    }
    return nullptr;
}

const RenderObject* Scene::find(uint32_t id) const {
    for (const auto& obj : objects_) {
        if (obj.id == id) return &obj;
    }
    return nullptr;
}

int Scene::index_of(uint32_t id) const {
    for (int i = 0; i < static_cast<int>(objects_.size()); ++i) {
        if (objects_[i].id == id) return i;
    }
    return -1;
}

void Scene::compute_world_matrices() {
    std::vector<bool> computed(objects_.size(), false);
    auto remaining = objects_.size();

    // Pass 1: roots
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        if (objects_[i].parent_id == -1) {
            objects_[i].world_matrix_ = objects_[i].transform.matrix();
            computed[i] = true;
            --remaining;
        }
    }

    // Subsequent passes: children whose parent is already computed
    while (remaining > 0) {
        auto prev = remaining;
        for (std::size_t i = 0; i < objects_.size(); ++i) {
            if (computed[i]) continue;
            int pi = index_of(static_cast<uint32_t>(objects_[i].parent_id));
            if (pi < 0) {
                // Orphaned reference — treat as root
                objects_[i].parent_id = -1;
                objects_[i].world_matrix_ = objects_[i].transform.matrix();
                computed[i] = true;
                --remaining;
            } else if (computed[pi]) {
                objects_[i].world_matrix_ =
                    objects_[pi].world_matrix_ * objects_[i].transform.matrix();
                computed[i] = true;
                --remaining;
            }
        }
        if (remaining == prev) {
            // Cycle — break all remaining
            for (std::size_t i = 0; i < objects_.size(); ++i) {
                if (!computed[i]) {
                    objects_[i].parent_id = -1;
                    objects_[i].world_matrix_ = objects_[i].transform.matrix();
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
}

void Scene::save(const std::string& path) const {
    nlohmann::json j;
    j["next_id"] = next_id_;
    j["objects"] = nlohmann::json::array();

    for (const auto& obj : objects_) {
        nlohmann::json jobj;
        jobj["id"] = obj.id;
        jobj["parent_id"] = obj.parent_id;
        jobj["name"] = obj.name;
        jobj["source"] = obj.source;
        jobj["material"] = obj.material_name;
        if (obj.material) {
            const auto& bcf = obj.material->params().base_color_factor;
            if (bcf.r != 1.0f || bcf.g != 1.0f || bcf.b != 1.0f) {
                jobj["color"] = {bcf.r, bcf.g, bcf.b};
            }
        }
        jobj["position"] = {obj.transform.position.x, obj.transform.position.y, obj.transform.position.z};
        jobj["rotation"] = {obj.transform.rotation.x, obj.transform.rotation.y, obj.transform.rotation.z};
        jobj["scale"] = {obj.transform.scale.x, obj.transform.scale.y, obj.transform.scale.z};

        if (obj.rigidbody) {
            auto& rb = *obj.rigidbody;
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
        if (obj.collider) {
            auto& col = *obj.collider;
            jobj["collider"] = {
                {"shape", static_cast<int>(col.shape)},
                {"half_extents", {col.half_extents.x, col.half_extents.y, col.half_extents.z}},
                {"radius", col.radius},
                {"height", col.height},
                {"offset", {col.offset.x, col.offset.y, col.offset.z}}
            };
        }

        if (!obj.components.empty()) {
            jobj["components"] = nlohmann::json::array();
            for (const auto& comp : obj.components) {
                nlohmann::json jcomp;
                jcomp["type"] = comp.type_name;
                jcomp["data"] = comp.serialized;
                jobj["components"].push_back(jcomp);
            }
        }

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
    auto default_tex = resources.load_texture(
        base_path::resolve_asset("assets/textures/test.png", project));

    uint32_t max_id = 0;

    for (const auto& jobj : j["objects"]) {
        std::string name = jobj.value("name", "Object");
        std::string source = jobj.value("source", "");
        std::string mat_name = jobj.value("material", "default");

        ModelHandle model_handle;
        if (source.starts_with("primitive:")) {
            std::string type = source.substr(10);
            model_handle = resources.create_primitive(type);
        } else if (!source.empty()) {
            model_handle = resources.load_model(base_path::resolve_asset(source, project));
        }

        if (!model_handle.valid()) continue;

        MaterialHandle mat_handle;
        if (jobj.contains("color")) {
            auto& c = jobj["color"];
            PbrParams params{};
            params.base_color_factor = {c[0].get<float>(), c[1].get<float>(),
                                        c[2].get<float>(), 1.0f};
            std::string color_mat_name = mat_name + "_color_" + std::to_string(jobj.value("id", 0));
            mat_handle = resources.create_material(color_mat_name,
                resources.default_white_handle(), {}, {}, {}, params);
        } else {
            mat_handle = resources.create_material(mat_name, default_tex);
        }

        auto* model = resources.get(model_handle);
        if (!source.starts_with("primitive:") && model && !model->textures().empty()) {
            mat_handle = resources.create_material_from_texture(
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

        uint32_t id = scene.add(model_handle, mat_handle, resources,
                                transform, name, source, mat_name);

        // Override with loaded ID if present (backward compat: use assigned ID)
        uint32_t loaded_id = jobj.value("id", 0u);
        int32_t loaded_parent = jobj.value("parent_id", -1);
        if (loaded_id > 0) {
            scene.objects().back().id = loaded_id;
            if (loaded_id > max_id) max_id = loaded_id;
        } else {
            if (id > max_id) max_id = id;
        }
        scene.objects().back().parent_id = loaded_parent;

        // Load rigidbody descriptor
        if (jobj.contains("rigidbody")) {
            auto& jrb = jobj["rigidbody"];
            RigidbodyDesc rb;
            rb.body_type = static_cast<BodyType>(jrb.value("body_type", 2));
            rb.mass = jrb.value("mass", 1.0f);
            rb.friction = jrb.value("friction", 0.5f);
            rb.restitution = jrb.value("restitution", 0.3f);
            rb.linear_damping = jrb.value("linear_damping", 0.05f);
            rb.angular_damping = jrb.value("angular_damping", 0.05f);
            rb.gravity_enabled = jrb.value("gravity_enabled", true);
            scene.objects().back().rigidbody = rb;
        }
        // Load collider descriptor
        if (jobj.contains("collider")) {
            auto& jcol = jobj["collider"];
            ColliderDesc col;
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
            scene.objects().back().collider = col;
        }

        // Load component data as raw JSON (ScriptManager will instantiate later)
        if (jobj.contains("components")) {
            for (const auto& jcomp : jobj["components"]) {
                ComponentEntry comp;
                comp.type_name = jcomp.value("type", "");
                if (jcomp.contains("data")) {
                    comp.serialized = jcomp["data"];
                }
                scene.objects().back().components.push_back(std::move(comp));
            }
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
