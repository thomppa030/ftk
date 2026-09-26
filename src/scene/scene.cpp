#include "scene/scene.hpp"
#include "scene/transform_blend.hpp"
#include "core/thread_pool.hpp"
#include "core/profiler.hpp"
#include "scripting/fjell/component.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <limits>
#include <utility>

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

Scene::~Scene() {
    // The nodes' components live in pools_, which is destroyed after them
    // only if they go first.
    objects_.clear();
    pending_spawn_.clear();
}

ComponentPools& Scene::pools() {
    if (!pools_) pools_ = std::make_unique<ComponentPools>();
    return *pools_;
}

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
    node->components.attach(pools(), id);

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
    if (tracking_added_) added_.push_back(id);
    ++structure_revision_;
    return id;
}

std::vector<uint32_t> Scene::flush_spawned() {
    if (pending_spawn_.empty()) return {};
    std::vector<uint32_t> spawned;
    spawned.reserve(pending_spawn_.size());
    for (auto& node : pending_spawn_) {
        spawned.push_back(node->id);
        if (tracking_added_) added_.push_back(node->id);
        id_to_index_[node->id] = static_cast<int>(objects_.size());
        objects_.push_back(std::move(node));
    }
    pending_spawn_.clear();
    index_dirty_ = true;
    dirty_ = true;
    ++structure_revision_;
    return spawned;
}

void Scene::track_added(bool on) {
    tracking_added_ = on;
    added_.clear();
}

std::vector<uint32_t> Scene::take_added() {
    return std::exchange(added_, {});
}

void Scene::remove(uint32_t id) {
    (void)extract(id);
}

std::unique_ptr<Node> Scene::extract(uint32_t id) {
    if (index_dirty_) { rebuild_id_index(); index_dirty_ = false; }
    auto map_it = id_to_index_.find(id);
    if (map_it == id_to_index_.end()) return nullptr;
    for (auto& obj : objects_) {
        if (obj->parent_id == static_cast<int32_t>(id)) {
            obj->parent_id = -1;
        }
    }
    id_generation_[id]++;
    const int idx = map_it->second;
    auto result = std::move(objects_[static_cast<std::size_t>(idx)]);
    objects_.erase(objects_.begin() + idx);
    result->components.detach();
    rebuild_id_index();
    dirty_ = true;
    ++structure_revision_;
    return result;
}

void Scene::restore(std::unique_ptr<Node> node, std::size_t index) {
    if (node->id >= next_id_) next_id_ = node->id + 1;
    // Keep existing generation — node is coming back
    node->components.attach(pools(), node->id);
    index = std::min(index, objects_.size());
    objects_.insert(objects_.begin() + static_cast<std::ptrdiff_t>(index), std::move(node));
    rebuild_id_index();
    dirty_ = true;
    ++structure_revision_;
}

int Scene::index_of(uint32_t id) {
    if (index_dirty_) { rebuild_id_index(); index_dirty_ = false; }
    auto it = id_to_index_.find(id);
    return it != id_to_index_.end() ? it->second : -1;
}

void Scene::move(uint32_t id, int32_t parent_id, uint32_t before_id) {
    const int from = index_of(id);
    if (from < 0 || before_id == id) return;
    // Where `before` stands once this node is out of the list.
    std::size_t to = objects_.size() - 1;
    if (before_id != 0) {
        const int before = index_of(before_id);
        if (before >= 0) to = static_cast<std::size_t>(before > from ? before - 1 : before);
    }
    move_to(id, parent_id, to);
}

void Scene::move_to(uint32_t id, int32_t parent_id, std::size_t index) {
    const int from = index_of(id);
    if (from < 0) return;
    reparent(id, parent_id);
    auto node = std::move(objects_[static_cast<std::size_t>(from)]);
    objects_.erase(objects_.begin() + from);
    index = std::min(index, objects_.size());
    objects_.insert(objects_.begin() + static_cast<std::ptrdiff_t>(index), std::move(node));
    rebuild_id_index();
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
    ++structure_revision_;
}

uint32_t Scene::generation_of(uint32_t id) const {
    auto it = id_generation_.find(id);
    return it != id_generation_.end() ? it->second : 0;
}

void Scene::queue_destroy(uint32_t id) {
    // The node and everything under it, each once however many times it is
    // asked for: a parent and its child can both be destroyed in one frame.
    const std::size_t first = pending_destroy_.size();
    if (std::ranges::find(pending_destroy_, id) == pending_destroy_.end()) {
        pending_destroy_.push_back(id);
    }
    for (std::size_t i = first; i < pending_destroy_.size(); ++i) {
        const auto parent = static_cast<int32_t>(pending_destroy_[i]);
        for (const auto& obj : objects_) {
            if (obj->parent_id == parent &&
                std::ranges::find(pending_destroy_, obj->id) == pending_destroy_.end()) {
                pending_destroy_.push_back(obj->id);
            }
        }
    }
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
    ++structure_revision_;
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

namespace {

/// Whether two transforms place a node identically.
[[nodiscard]] bool same_placement(const Transform& a, const Transform& b) {
    return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale;
}

/// Where the node is drawn. Each part of a node the last step moved is drawn
/// blended while it is still where that step left it. A part something
/// outside the simulation has changed since (a script turning a character in
/// its tick, a teleport, the network, the inspector) is drawn as it now is:
/// a turn does not stop a walk from blending, and a jump is not blended
/// across.
[[nodiscard]] Transform drawn_transform(const Node3D& node) {
    if (!node.interpolated_) return node.transform;
    const Transform& now = node.transform;
    const Transform& end = node.step_end_transform_;
    const Transform& blend = node.interpolated_transform_;
    return Transform{
        .position = now.position == end.position ? blend.position : now.position,
        .rotation = now.rotation == end.rotation ? blend.rotation : now.rotation,
        .scale = now.scale == end.scale ? blend.scale : now.scale,
    };
}

} // namespace

void Scene::compute_world_matrices(ThreadPool* pool) {
    FJELL_PROFILE_SCOPE_N("compute_world_matrices");

    if (index_dirty_) {
        rebuild_id_index();
        index_dirty_ = false;
    }

    auto n = objects_.size();

    // Snapshot previous world matrices for motion vectors, then detect dirty
    // transforms. A node is drawn at its blended transform while the last
    // fixed step moved it, and at its transform otherwise.
    auto detect_dirty = [&](uint32_t /*chunk*/, uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            auto* spatial = objects_[i]->as_spatial();
            if (!spatial) continue;
            spatial->prev_world_matrix_ = spatial->world_matrix_;
            const Transform drawn = drawn_transform(*spatial);
            if (spatial->transform_dirty_ || !same_placement(drawn, spatial->prev_transform_)) {
                spatial->local_matrix_ = drawn.matrix();
                spatial->prev_transform_ = drawn;
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

glm::mat4 Scene::current_world_matrix(const Node3D& node) const {
    glm::mat4 world = node.transform.matrix();
    int32_t parent_id = node.parent_id;
    while (parent_id >= 0) {
        const auto* parent = find(static_cast<uint32_t>(parent_id));
        if (parent == nullptr) break;
        if (const auto* spatial = parent->as_spatial()) {
            world = spatial->transform.matrix() * world;
        }
        parent_id = parent->parent_id;
    }
    return world;
}

glm::mat4 Scene::parent_world_matrix(const Node3D& node) const {
    glm::mat4 parent_world{1.0f};
    int32_t parent_id = node.parent_id;
    while (parent_id >= 0) {
        const auto* parent = find(static_cast<uint32_t>(parent_id));
        if (parent == nullptr) break;
        if (const auto* spatial = parent->as_spatial()) {
            parent_world = spatial->transform.matrix() * parent_world;
        }
        parent_id = parent->parent_id;
    }
    return parent_world;
}

void Scene::set_world_position(Node3D& node, const glm::vec3& world_position) const {
    const glm::mat4 parent_world = parent_world_matrix(node);
    node.transform.position = glm::vec3{glm::inverse(parent_world) * glm::vec4{world_position, 1.0f}};
}

void Scene::teleport(Node3D& node, const glm::vec3& world_position,
                     const glm::quat& world_rotation) const {
    const glm::mat4 parent_world = parent_world_matrix(node);
    node.transform.position = glm::vec3{glm::inverse(parent_world) * glm::vec4{world_position, 1.0f}};
    // The parent's rotation, from its basis with the scale taken out.
    const glm::quat parent_rotation = glm::quat_cast(glm::mat3{
        glm::normalize(glm::vec3(parent_world[0])),
        glm::normalize(glm::vec3(parent_world[1])),
        glm::normalize(glm::vec3(parent_world[2]))});
    node.transform.rotation = glm::normalize(glm::inverse(parent_rotation) * world_rotation);

    // Arriving is the start and end of the step as far as blending is
    // concerned, so it is never drawn crossing the distance it jumped. Moved
    // inside a step, whatever else the step does to it blends from here.
    node.step_start_transform_ = node.transform;
    node.step_end_transform_ = node.transform;
    node.interpolated_ = false;
}

// ── Blending between fixed steps ────────────────────────────────────────

void Scene::begin_fixed_step(std::uint64_t index) {
    for (auto& obj : objects_) {
        auto* spatial = obj->as_spatial();
        if (spatial == nullptr) continue;
        spatial->step_start_transform_ = spatial->transform;
        spatial->step_start_index_ = index;
    }
}

void Scene::end_fixed_steps(std::uint64_t last_index) {
    for (auto& obj : objects_) {
        auto* spatial = obj->as_spatial();
        if (spatial == nullptr) continue;
        spatial->step_end_transform_ = spatial->transform;
        // Spawned during the last step, it has no start in that step to blend
        // from, and appears where it was put.
        spatial->interpolated_ =
            spatial->step_start_index_ == last_index &&
            !same_placement(spatial->step_start_transform_, spatial->step_end_transform_);
    }
}

void Scene::interpolate(float alpha) {
    for (auto& obj : objects_) {
        auto* spatial = obj->as_spatial();
        if (spatial == nullptr || !spatial->interpolated_) continue;
        spatial->interpolated_transform_ =
            blend(spatial->step_start_transform_, spatial->step_end_transform_, alpha);
    }
}

void Scene::clear_interpolation() {
    for (auto& obj : objects_) {
        auto* spatial = obj->as_spatial();
        if (spatial == nullptr) continue;
        spatial->interpolated_ = false;
        spatial->step_start_index_ = std::numeric_limits<std::uint64_t>::max();
    }
}

glm::mat4 Scene::interpolated_world_matrix(const Node3D& node) const {
    glm::mat4 world = drawn_transform(node).matrix();
    int32_t parent_id = node.parent_id;
    while (parent_id >= 0) {
        const auto* parent = find(static_cast<uint32_t>(parent_id));
        if (parent == nullptr) break;
        if (const auto* spatial = parent->as_spatial()) {
            world = drawn_transform(*spatial).matrix() * world;
        }
        parent_id = parent->parent_id;
    }
    return world;
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
