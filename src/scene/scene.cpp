#include "scene/scene.hpp"
#include "scene/node_serializer.hpp"
#include "animation/animator_component.hpp"
#include "core/input_component.hpp"
#include "ui/game_ui/ui_widget_component.hpp"
#include "audio/audio_component.hpp"
#include "physics/physics_types.hpp"
#include "renderer/arrow_component.hpp"
#include "renderer/mesh_component.hpp"
#include "renderer/particle_emitter.hpp"
#include "renderer/vfx_asset.hpp"
#include "renderer/vfx_emitter.hpp"
#include "renderer/skeletal_mesh_component.hpp"
#include "renderer/sprite_anim_component.hpp"
#include "renderer/sprite_desc.hpp"
#include "renderer/tilemap_component.hpp"
#include "core/thread_pool.hpp"
#include "core/profiler.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/skeletal_model.hpp"
#include "renderer/texture.hpp"
#include "scripting/fjell/component.hpp"
#include "core/base_path.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <cstring>

#include <algorithm>
#include <fstream>

namespace fjell {

// ── Out-of-line special members (need complete Component types) ─────────

ScriptEntry::ScriptEntry() = default;
ScriptEntry::~ScriptEntry() = default;
ScriptEntry::ScriptEntry(ScriptEntry&&) noexcept = default;
ScriptEntry& ScriptEntry::operator=(ScriptEntry&&) noexcept = default;

// ── Node ────────────────────────────────────────────────────────────────

Node::Node() = default;
Node::~Node() = default;
Node::Node(Node&&) noexcept = default;
Node& Node::operator=(Node&&) noexcept = default;

void Node::clone_base_into(Node& target) const {
    target.id = id;
    target.parent_id = parent_id;
    target.name = name;
    target.source = source;
    target.preset_source = preset_source;
    if (const auto* mc = components.get<MeshComponent>()) {
        target.components.add<MeshComponent>(*mc);
    }
    for (const auto& s : scripts) {
        ScriptEntry entry;
        entry.type_name = s.type_name;
        entry.serialized = s.serialized;
        target.scripts.push_back(std::move(entry));
    }
    if (const auto* rb = components.get<RigidbodyComponent>()) {
        target.components.add<RigidbodyComponent>(*rb);
    }
    if (const auto* col = components.get<ColliderComponent>()) {
        target.components.add<ColliderComponent>(*col);
    }
    if (const auto* em = components.get<EmitterComponent>()) {
        target.components.add<EmitterComponent>(*em);
    }
    if (const auto* sp = components.get<SpriteComponent>()) {
        target.components.add<SpriteComponent>(*sp);
    }
    if (const auto* sa = components.get<SpriteAnimComponent>()) {
        auto& copy = target.components.add<SpriteAnimComponent>();
        copy.animset_path = sa->animset_path;
        copy.animations = sa->animations;
        copy.default_animation = sa->default_animation;
        copy.playback_speed = sa->playback_speed;
    }
    if (const auto* tm = components.get<TilemapComponent>()) {
        auto& copy = target.components.add<TilemapComponent>();
        copy.tileset_texture = tm->tileset_texture;
        copy.tile_size = tm->tile_size;
        copy.map_size = tm->map_size;
        copy.world_tile_size = tm->world_tile_size;
        copy.sort_layer = tm->sort_layer;
        copy.pixel_art = tm->pixel_art;
        copy.tiles = tm->tiles;
    }
    if (const auto* arr = components.get<ArrowComponent>()) {
        target.components.add<ArrowComponent>(*arr);
    }
    if (const auto* au = components.get<AudioSourceComponent>()) {
        auto& copy = target.components.add<AudioSourceComponent>();
        copy.clip_path = au->clip_path;
        copy.volume = au->volume;
        copy.pitch = au->pitch;
        copy.min_distance = au->min_distance;
        copy.max_distance = au->max_distance;
        copy.loop = au->loop;
        copy.spatial = au->spatial;
        copy.play_on_start = au->play_on_start;
    }
    if (const auto* cc = components.get<CharacterComponent>()) {
        auto& copy = target.components.add<CharacterComponent>();
        copy.capsule_radius = cc->capsule_radius;
        copy.capsule_height = cc->capsule_height;
        copy.max_slope_angle = cc->max_slope_angle;
        copy.mass = cc->mass;
    }
    if (const auto* wc = components.get<UiWidgetComponent>()) {
        target.components.add<UiWidgetComponent>(*wc);
    }
    if (const auto* ic = components.get<InputComponent>()) {
        target.components.add<InputComponent>(*ic);
    }
    if (const auto* smc = components.get<SkeletalMeshComponent>()) {
        target.components.add<SkeletalMeshComponent>(*smc);
    }
    if (const auto* anim = components.get<AnimatorComponent>()) {
        auto& copy = target.components.add<AnimatorComponent>();
        copy.clip_paths = anim->clip_paths;
        copy.animset_path = anim->animset_path;
        copy.clips = anim->clips;
        // Init from SkeletalModel's embedded skeleton
        if (const auto* smc = components.get<SkeletalMeshComponent>()) {
            if (smc->skeletal_model && smc->skeletal_model->skeleton()) {
                copy.init(smc->skeletal_model->skeleton());
            }
        }
        // Re-register clips in the clip map
        for (size_t i = 0; i < copy.clips.size(); ++i) {
            if (i < copy.clip_paths.size()) {
                copy.set_clip(copy.clip_paths[i], copy.clips[i].get());
            }
            if (i == 0) {
                copy.play(copy.clips[i].get());
            }
        }
        if (!copy.animset_path.empty()) {
            copy.load_state_machine(copy.animset_path);
        }
    }
}

std::unique_ptr<Node> Node::clone() const {
    auto copy = std::make_unique<Node>();
    clone_base_into(*copy);
    return copy;
}

// ── Node3D ──────────────────────────────────────────────────────────────

Node3D::Node3D() { is_spatial_ = true; }
Node3D::~Node3D() = default;
Node3D::Node3D(Node3D&&) noexcept = default;
Node3D& Node3D::operator=(Node3D&&) noexcept = default;

void Node3D::clone_3d_into(Node3D& target) const {
    clone_base_into(target);
    target.transform = transform;
    target.world_matrix_ = world_matrix_;
}

std::unique_ptr<Node> Node3D::clone() const {
    auto copy = std::make_unique<Node3D>();
    clone_3d_into(*copy);
    return copy;
}

// ── Scene ───────────────────────────────────────────────────────────────

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
    id_to_index_[id] = static_cast<int>(objects_.size());
    objects_.push_back(std::move(obj));
    id_generation_[id] = 1;
    dirty_ = true;
    return id;
}

uint32_t Scene::add_empty(Transform transform, const std::string& name) {
    uint32_t id = next_id_++;
    auto obj = std::make_unique<Node3D>();
    obj->id = id;
    obj->transform = transform;
    obj->name = name;
    id_to_index_[id] = static_cast<int>(objects_.size());
    objects_.push_back(std::move(obj));
    id_generation_[id] = 1;
    dirty_ = true;
    return id;
}

uint32_t Scene::add_folder(const std::string& name) {
    uint32_t id = next_id_++;
    auto obj = std::make_unique<Node>();
    obj->id = id;
    obj->name = name;
    id_to_index_[id] = static_cast<int>(objects_.size());
    objects_.push_back(std::move(obj));
    id_generation_[id] = 1;
    dirty_ = true;
    return id;
}

uint32_t Scene::add_node(std::unique_ptr<Node> node) {
    if (node->id == 0) node->id = next_id_++;
    if (node->id >= next_id_) next_id_ = node->id + 1;
    uint32_t id = node->id;
    id_to_index_[id] = static_cast<int>(objects_.size());
    objects_.push_back(std::move(node));
    if (!id_generation_.contains(id)) {
        id_generation_[id] = 1;
    }
    dirty_ = true;
    return id;
}

void Scene::remove(uint32_t id) {
    for (auto& obj : objects_) {
        if (obj->parent_id == static_cast<int32_t>(id)) {
            obj->parent_id = -1;
        }
    }
    if (index_dirty_) { rebuild_id_index(); index_dirty_ = false; }
    auto map_it = id_to_index_.find(id);
    if (map_it == id_to_index_.end()) return;

    int idx = map_it->second;
    id_generation_[id]++;
    // Swap-and-pop: O(1) removal instead of O(n) shift
    if (idx < static_cast<int>(objects_.size()) - 1) {
        uint32_t back_id = objects_.back()->id;
        std::swap(objects_[idx], objects_.back());
        id_to_index_[back_id] = idx;
    }
    objects_.pop_back();
    id_to_index_.erase(map_it);
    dirty_ = true;
}

std::unique_ptr<Node> Scene::extract(uint32_t id) {
    for (auto& obj : objects_) {
        if (obj->parent_id == static_cast<int32_t>(id)) {
            obj->parent_id = -1;
        }
    }
    if (index_dirty_) { rebuild_id_index(); index_dirty_ = false; }
    auto map_it = id_to_index_.find(id);
    id_generation_[id]++;

    int idx = map_it->second;
    auto result = std::move(objects_[idx]);
    // Swap-and-pop
    if (idx < static_cast<int>(objects_.size()) - 1) {
        objects_[idx] = std::move(objects_.back());
        id_to_index_[objects_[idx]->id] = idx;
    }
    objects_.pop_back();
    id_to_index_.erase(map_it);
    dirty_ = true;
    return result;
}

void Scene::restore(std::unique_ptr<Node> node) {
    if (node->id >= next_id_) next_id_ = node->id + 1;
    // Keep existing generation — node is coming back
    uint32_t id = node->id;
    id_to_index_[id] = static_cast<int>(objects_.size());
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
    id_generation_.clear();
    pending_destroy_.clear();
    id_to_index_.clear();
    index_dirty_ = false;
}

uint32_t Scene::generation_of(uint32_t id) const {
    auto it = id_generation_.find(id);
    return it != id_generation_.end() ? it->second : 0;
}

void Scene::queue_destroy(uint32_t id) {
    pending_destroy_.push_back(id);
}

void Scene::flush_destroyed() {
    if (pending_destroy_.empty()) return;

    if (index_dirty_) { rebuild_id_index(); index_dirty_ = false; }

    for (uint32_t doomed : pending_destroy_) {
        // Reparent children to root
        for (auto& obj : objects_) {
            if (obj->parent_id == static_cast<int32_t>(doomed)) {
                obj->parent_id = -1;
            }
        }
        // Bump generation so WeakRefs go stale
        id_generation_[doomed]++;
        // Swap-and-pop removal via index map
        auto map_it = id_to_index_.find(doomed);
        if (map_it == id_to_index_.end()) continue;
        int idx = map_it->second;
        if (idx < static_cast<int>(objects_.size()) - 1) {
            uint32_t back_id = objects_.back()->id;
            std::swap(objects_[idx], objects_.back());
            id_to_index_[back_id] = idx;
        }
        objects_.pop_back();
        id_to_index_.erase(map_it);
    }

    pending_destroy_.clear();
    dirty_ = true;
}

void Scene::rebuild_id_index() {
    id_to_index_.clear();
    id_to_index_.reserve(objects_.size());
    for (int i = 0; i < static_cast<int>(objects_.size()); ++i) {
        id_to_index_[objects_[i]->id] = i;
    }
}

Node* Scene::find(uint32_t id) {
    if (index_dirty_) { rebuild_id_index(); index_dirty_ = false; }
    auto it = id_to_index_.find(id);
    if (it != id_to_index_.end()) {
        return objects_[it->second].get();
    }
    return nullptr;
}

const Node* Scene::find(uint32_t id) const {
    auto it = id_to_index_.find(id);
    if (it != id_to_index_.end()) {
        return objects_[it->second].get();
    }
    return nullptr;
}

int Scene::index_of(uint32_t id) const {
    auto it = id_to_index_.find(id);
    if (it != id_to_index_.end()) return it->second;
    return -1;
}

std::vector<const Node*> Scene::collect_subtree(uint32_t root_id) const {
    std::vector<const Node*> result;
    const Node* root = find(root_id);
    if (!root) return result;

    // Build parent→children map once (O(n)), then BFS is O(subtree_size)
    std::unordered_map<int32_t, std::vector<const Node*>> children_map;
    children_map.reserve(objects_.size());
    for (const auto& obj : objects_) {
        if (obj->parent_id >= 0) {
            children_map[obj->parent_id].push_back(obj.get());
        }
    }

    result.push_back(root);
    for (size_t i = 0; i < result.size(); ++i) {
        auto it = children_map.find(static_cast<int32_t>(result[i]->id));
        if (it != children_map.end()) {
            result.insert(result.end(), it->second.begin(), it->second.end());
        }
    }

    return result;
}

void Scene::compute_world_matrices(ThreadPool* pool) {
    FJELL_PROFILE_SCOPE_N("compute_world_matrices");

    if (index_dirty_) {
        rebuild_id_index();
        index_dirty_ = false;
    }

    auto n = objects_.size();

    // Detect dirty transforms — skip non-spatial nodes (folders)
    auto detect_dirty = [&](uint32_t /*chunk*/, uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            auto* spatial = objects_[i]->as_spatial();
            if (!spatial) continue;
            if (spatial->transform_dirty_ ||
                spatial->transform.position != spatial->prev_transform_.position ||
                spatial->transform.rotation != spatial->prev_transform_.rotation ||
                spatial->transform.scale != spatial->prev_transform_.scale) {
                spatial->local_matrix_ = spatial->transform.matrix();
                spatial->prev_transform_ = spatial->transform;
                spatial->transform_dirty_ = false;
                spatial->world_dirty_ = true;
            } else {
                spatial->world_dirty_ = false;
            }
        }
    };

    if (pool && n > 64) {
        pool->parallel_for(0, static_cast<uint32_t>(n), detect_dirty);
    } else {
        detect_dirty(0, 0, static_cast<uint32_t>(n));
    }

    computed_buf_.resize(n);
    std::memset(computed_buf_.data(), 0, n);
    auto remaining = n;

    // Pass 1: roots and non-spatial nodes
    for (std::size_t i = 0; i < n; ++i) {
        auto* spatial = objects_[i]->as_spatial();
        if (!spatial) {
            computed_buf_[i] = 1;
            --remaining;
            continue;
        }
        if (objects_[i]->parent_id == -1) {
            if (spatial->world_dirty_) {
                spatial->world_matrix_ = spatial->local_matrix_;
            }
            computed_buf_[i] = 1;
            --remaining;
        }
    }

    // Subsequent passes: children whose parent is already computed
    while (remaining > 0) {
        auto prev = remaining;
        for (std::size_t i = 0; i < n; ++i) {
            if (computed_buf_[i]) continue;
            auto* spatial = objects_[i]->as_spatial();
            if (!spatial) {
                computed_buf_[i] = 1;
                --remaining;
                continue;
            }
            int pi = index_of(static_cast<uint32_t>(objects_[i]->parent_id));
            if (pi < 0) {
                objects_[i]->parent_id = -1;
                if (spatial->world_dirty_) {
                    spatial->world_matrix_ = spatial->local_matrix_;
                }
                computed_buf_[i] = 1;
                --remaining;
            } else if (computed_buf_[pi]) {
                auto* parent_spatial = objects_[pi]->as_spatial();
                if (parent_spatial) {
                    if (parent_spatial->world_dirty_) {
                        spatial->world_dirty_ = true;
                    }
                    if (spatial->world_dirty_) {
                        spatial->world_matrix_ =
                            parent_spatial->world_matrix_ * spatial->local_matrix_;
                    }
                } else {
                    // Parent is a folder — child's world = local
                    if (spatial->world_dirty_) {
                        spatial->world_matrix_ = spatial->local_matrix_;
                    }
                }
                computed_buf_[i] = 1;
                --remaining;
            }
        }
        if (remaining == prev) {
            for (std::size_t i = 0; i < n; ++i) {
                if (!computed_buf_[i]) {
                    objects_[i]->parent_id = -1;
                    if (auto* s = objects_[i]->as_spatial()) {
                        s->world_matrix_ = s->local_matrix_;
                        s->world_dirty_ = true;
                    }
                }
            }
            break;
        }
    }
}

void Scene::reparent(uint32_t child_id, int32_t new_parent_id) {
    auto* child = find(child_id);
    if (!child) return;

    auto* spatial = child->as_spatial();
    if (spatial) {
        glm::mat4 child_world = spatial->world_matrix_;
        child->parent_id = new_parent_id;

        glm::mat4 new_local = child_world;
        if (new_parent_id != -1) {
            const auto* parent = find(static_cast<uint32_t>(new_parent_id));
            const auto* parent_spatial = parent ? parent->as_spatial() : nullptr;
            if (parent_spatial) {
                new_local = glm::inverse(parent_spatial->world_matrix_) * child_world;
            }
        }
        spatial->transform = decompose_matrix(new_local);
    } else {
        child->parent_id = new_parent_id;
    }
    dirty_ = true;
}

Result<> Scene::save(const std::string& path) {
    nlohmann::json j;
    j["next_id"] = next_id_;
    j["objects"] = nlohmann::json::array();

    for (const auto& obj : objects_) {
        j["objects"].push_back(serialize_node(*obj));
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
        return make_error("Failed to save scene: " + path);
    }
    file << j.dump(2);
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

    Scene scene;
    uint32_t max_id = 0;

    for (const auto& jobj : j["objects"]) {
        auto node = deserialize_node(jobj, resources, project);
        if (!node) continue;

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
                pl.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
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
                sl.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
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

    FJELL_CORE_INFO("Scene loaded: {} objects from {}", scene.object_count(), path);
    return scene;
}

} // namespace fjell
