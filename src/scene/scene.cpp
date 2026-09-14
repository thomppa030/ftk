#include "scene/scene.hpp"
#include "core/thread_pool.hpp"
#include "core/profiler.hpp"
#include "scripting/fjell/component.hpp"

#include <algorithm>

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
    components.clone_into(target.components);
    for (const auto& s : scripts) {
        ScriptEntry entry;
        entry.type_name = s.type_name;
        entry.serialized = s.serialized;
        target.scripts.push_back(std::move(entry));
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

uint32_t Scene::add_empty(Transform transform, const std::string& name) {
    uint32_t id = next_id_++;
    auto obj = std::make_unique<Node3D>();
    obj->id = id;
    obj->transform = transform;
    obj->name = name;
    return add_node(std::move(obj));
}

uint32_t Scene::add_folder(const std::string& name) {
    uint32_t id = next_id_++;
    auto obj = std::make_unique<Node>();
    obj->id = id;
    obj->name = name;
    return add_node(std::move(obj));
}

uint32_t Scene::add_node(std::unique_ptr<Node> node) {
    if (node->id == 0) node->id = next_id_++;
    if (node->id >= next_id_) next_id_ = node->id + 1;
    uint32_t id = node->id;
    if (!id_generation_.contains(id)) {
        id_generation_[id] = 1;
    }
    dirty_ = true;

    // Adding during a tick would invalidate the iterator the caller is walking
    // over objects_; defer until flush_spawned() runs after the loop. The id is
    // already assigned, so callers (and networking) can use it immediately.
    if (iteration_depth_ > 0) {
        pending_spawn_.push_back(std::move(node));
        index_dirty_ = true;
        return id;
    }

    id_to_index_[id] = static_cast<int>(objects_.size());
    objects_.push_back(std::move(node));
    return id;
}

std::vector<uint32_t> Scene::flush_spawned() {
    if (pending_spawn_.empty()) return {};
    std::vector<uint32_t> spawned;
    spawned.reserve(pending_spawn_.size());
    for (auto& node : pending_spawn_) {
        spawned.push_back(node->id);
        id_to_index_[node->id] = static_cast<int>(objects_.size());
        objects_.push_back(std::move(node));
    }
    pending_spawn_.clear();
    index_dirty_ = true;
    dirty_ = true;
    return spawned;
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
    // A node spawned during a tick is real and has its id, it just has not
    // been moved into objects_ yet. Callers are told they can use the id
    // immediately, so looking it up has to work before the flush.
    for (auto& pending : pending_spawn_) {
        if (pending->id == id) return pending.get();
    }
    return nullptr;
}

const Node* Scene::find(uint32_t id) const {
    auto it = id_to_index_.find(id);
    if (it != id_to_index_.end()) {
        return objects_[it->second].get();
    }
    for (const auto& pending : pending_spawn_) {
        if (pending->id == id) return pending.get();
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

    // Children come only from objects_, so that size bounds the queue closely
    // enough to avoid regrowing it on the way down the tree.
    result.reserve(objects_.size());

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

    // Snapshot previous world matrices for motion vectors, then detect dirty transforms
    auto detect_dirty = [&](uint32_t /*chunk*/, uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            auto* spatial = objects_[i]->as_spatial();
            if (!spatial) continue;
            spatial->prev_world_matrix_ = spatial->world_matrix_;
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

} // namespace fjell
