#pragma once

#include "core/command.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fjell {

// ── Transform ────────────────────────────────────────────────────────────

class TransformCommand : public Command {
public:
    TransformCommand(Scene& scene, uint32_t id,
                     const Transform& old_t, const Transform& new_t,
                     std::string name)
        : scene_{scene}, id_{id}
        , old_transform_{old_t}, new_transform_{new_t}
        , name_{std::move(name)} {}

    void execute() override {
        if (auto* spatial = scene_.find_spatial(id_)) {
            spatial->transform = new_transform_;
            scene_.mark_dirty();
        }
    }
    void undo() override {
        if (auto* spatial = scene_.find_spatial(id_)) {
            spatial->transform = old_transform_;
            scene_.mark_dirty();
        }
    }
    [[nodiscard]] std::string description() const override {
        auto close = [](float a, float b) { return std::abs(a - b) < 0.001f; };
        auto vec3_close = [&](const glm::vec3& a, const glm::vec3& b) {
            return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z);
        };

        bool moved = !vec3_close(old_transform_.position, new_transform_.position);
        bool scaled = !vec3_close(old_transform_.scale, new_transform_.scale);
        bool rotated = !moved && !scaled; // if nothing else changed, assume rotation

        auto fmt_vec3 = [](const glm::vec3& v) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "(%.1f, %.1f, %.1f)", v.x, v.y, v.z);
            return std::string(buf);
        };

        if (moved && !scaled) {
            return "Move " + name_ + " \x01" + fmt_vec3(old_transform_.position)
                   + "\x02 -> \x03" + fmt_vec3(new_transform_.position) + "\x04";
        }
        if (scaled && !moved) {
            return "Scale " + name_ + " \x01" + fmt_vec3(old_transform_.scale)
                   + "\x02 -> \x03" + fmt_vec3(new_transform_.scale) + "\x04";
        }
        if (rotated) return "Rotate " + name_;
        return "Transform " + name_;
    }

private:
    Scene& scene_;
    uint32_t id_;
    Transform old_transform_;
    Transform new_transform_;
    std::string name_;
};

// ── Rename ───────────────────────────────────────────────────────────────

class RenameCommand : public Command {
public:
    RenameCommand(Scene& scene, uint32_t id,
                  std::string old_name, std::string new_name)
        : scene_{scene}, id_{id}
        , old_name_{std::move(old_name)}, new_name_{std::move(new_name)} {}

    void execute() override {
        if (auto* obj = scene_.find(id_)) {
            obj->name = new_name_;
            scene_.mark_dirty();
        }
    }
    void undo() override {
        if (auto* obj = scene_.find(id_)) {
            obj->name = old_name_;
            scene_.mark_dirty();
        }
    }
    [[nodiscard]] std::string description() const override {
        return "Rename '" + old_name_ + "' -> '" + new_name_ + "'";
    }

private:
    Scene& scene_;
    uint32_t id_;
    std::string old_name_;
    std::string new_name_;
};

// ── Add Object (capture-after-creation pattern) ─────────────────────────

class AddObjectCommand : public Command {
public:
    AddObjectCommand(Scene& scene, uint32_t id, uint32_t* selected_id)
        : scene_{scene}, id_{id}, selected_id_{selected_id} {
        // Snapshot immediately — the object already exists in the scene
        if (auto* obj = scene_.find(id_)) {
            snapshot_ = obj->clone();
            name_ = obj->name;
        }
    }

    void execute() override {
        if (first_) {
            first_ = false;
            return; // object already exists on first execute
        }
        scene_.restore(std::move(snapshot_));
        // Re-snapshot for next redo cycle
        if (auto* obj = scene_.find(id_)) {
            snapshot_ = obj->clone();
        }
        *selected_id_ = id_;
    }
    void undo() override {
        snapshot_ = scene_.extract(id_);
        if (*selected_id_ == id_) *selected_id_ = 0;
    }
    [[nodiscard]] std::string description() const override {
        return "Add " + name_;
    }

private:
    Scene& scene_;
    uint32_t id_;
    uint32_t* selected_id_;
    std::unique_ptr<Node> snapshot_;
    std::string name_;
    bool first_{true};
};

// ── Remove Object ────────────────────────────────────────────────────────

class RemoveObjectCommand : public Command {
public:
    RemoveObjectCommand(Scene& scene, uint32_t id, uint32_t* selected_id,
                        std::function<void()> pre_remove = {})
        : scene_{scene}, id_{id}, selected_id_{selected_id}
        , pre_remove_{std::move(pre_remove)} {
        // Capture children's parent links before removal
        if (auto* obj = scene_.find(id_)) {
            name_ = obj->name;
        }
        for (const auto& obj : scene_.objects()) {
            if (obj->parent_id == static_cast<int32_t>(id_)) {
                orphaned_children_.push_back({obj->id, obj->parent_id});
            }
        }
    }

    void execute() override {
        if (pre_remove_) pre_remove_();
        snapshot_ = scene_.extract(id_);
        if (*selected_id_ == id_) *selected_id_ = 0;
    }
    void undo() override {
        scene_.restore(std::move(snapshot_));
        // Re-snapshot for next execute cycle
        if (auto* obj = scene_.find(id_)) {
            snapshot_ = obj->clone();
        }
        // Restore child parent links
        for (const auto& [child_id, parent_id] : orphaned_children_) {
            if (auto* child = scene_.find(child_id)) {
                child->parent_id = parent_id;
            }
        }
        *selected_id_ = id_;
    }
    [[nodiscard]] std::string description() const override {
        return "Delete " + name_;
    }

private:
    Scene& scene_;
    uint32_t id_;
    uint32_t* selected_id_;
    std::function<void()> pre_remove_;
    std::unique_ptr<Node> snapshot_;
    std::string name_;
    std::vector<std::pair<uint32_t, int32_t>> orphaned_children_;
};

// ── Reparent ─────────────────────────────────────────────────────────────

class ReparentCommand : public Command {
public:
    ReparentCommand(Scene& scene, uint32_t child_id,
                    int32_t old_parent, int32_t new_parent,
                    std::string name)
        : scene_{scene}, child_id_{child_id}
        , old_parent_{old_parent}, new_parent_{new_parent}
        , name_{std::move(name)} {}

    void execute() override { scene_.reparent(child_id_, new_parent_); }
    void undo() override { scene_.reparent(child_id_, old_parent_); }
    [[nodiscard]] std::string description() const override {
        return "Reparent " + name_;
    }

private:
    Scene& scene_;
    uint32_t child_id_;
    int32_t old_parent_;
    int32_t new_parent_;
    std::string name_;
};

// ── Point Light ──────────────────────────────────────────────────────────

class AddPointLightCommand : public Command {
public:
    explicit AddPointLightCommand(Scene& scene) : scene_{scene} {}
    void execute() override {
        scene_.add_point_light(light_);
        index_ = static_cast<int>(scene_.point_lights().size()) - 1;
    }
    void undo() override {
        light_ = scene_.point_lights()[index_];
        scene_.remove_point_light(index_);
    }
    [[nodiscard]] std::string description() const override { return "Add Point Light"; }

private:
    Scene& scene_;
    PointLight light_{};
    int index_{-1};
};

class RemovePointLightCommand : public Command {
public:
    RemovePointLightCommand(Scene& scene, int index)
        : scene_{scene}, index_{index}
        , light_{scene.point_lights()[index]} {}
    void execute() override { scene_.remove_point_light(index_); }
    void undo() override { scene_.insert_point_light(index_, light_); }
    [[nodiscard]] std::string description() const override { return "Remove Point Light"; }

private:
    Scene& scene_;
    int index_;
    PointLight light_;
};

// ── Spot Light ───────────────────────────────────────────────────────────

class AddSpotLightCommand : public Command {
public:
    explicit AddSpotLightCommand(Scene& scene) : scene_{scene} {}
    void execute() override {
        scene_.add_spot_light(light_);
        index_ = static_cast<int>(scene_.spot_lights().size()) - 1;
    }
    void undo() override {
        light_ = scene_.spot_lights()[index_];
        scene_.remove_spot_light(index_);
    }
    [[nodiscard]] std::string description() const override { return "Add Spot Light"; }

private:
    Scene& scene_;
    SpotLight light_{};
    int index_{-1};
};

class RemoveSpotLightCommand : public Command {
public:
    RemoveSpotLightCommand(Scene& scene, int index)
        : scene_{scene}, index_{index}
        , light_{scene.spot_lights()[index]} {}
    void execute() override { scene_.remove_spot_light(index_); }
    void undo() override { scene_.insert_spot_light(index_, light_); }
    [[nodiscard]] std::string description() const override { return "Remove Spot Light"; }

private:
    Scene& scene_;
    int index_;
    SpotLight light_;
};

// ── Duplicate Objects ────────────────────────────────────────────────────

class DuplicateCommand : public Command {
public:
    DuplicateCommand(Scene& scene, std::vector<uint32_t> source_ids,
                     uint32_t* selected_id)
        : scene_{scene}, selected_id_{selected_id} {
        // Clone each source node with a new ID
        for (uint32_t id : source_ids) {
            const auto* src = scene_.find(id);
            if (!src) continue;
            auto copy = src->clone();
            copy->id = 0; // will be assigned by add_node
            copy->name = src->name + " (copy)";
            source_names_.push_back(src->name);
            clones_.push_back(std::move(copy));
        }
    }

    void execute() override {
        created_ids_.clear();
        for (auto& clone : clones_) {
            // Re-clone for redo cycles (after undo extracted the node)
            if (!clone) continue;
            uint32_t id = scene_.add_node(std::move(clone));
            created_ids_.push_back(id);
        }
        clones_.clear();
        if (!created_ids_.empty()) {
            *selected_id_ = created_ids_.back();
        }
    }

    void undo() override {
        // Extract in reverse order
        for (auto it = created_ids_.rbegin(); it != created_ids_.rend(); ++it) {
            clones_.push_back(scene_.extract(*it));
        }
        std::reverse(clones_.begin(), clones_.end());
        created_ids_.clear();
        *selected_id_ = 0;
    }

    [[nodiscard]] std::string description() const override {
        if (source_names_.size() == 1) return "Duplicate " + source_names_[0];
        return "Duplicate " + std::to_string(source_names_.size()) + " objects";
    }

    [[nodiscard]] const std::vector<uint32_t>& created_ids() const { return created_ids_; }

private:
    Scene& scene_;
    uint32_t* selected_id_;
    std::vector<std::unique_ptr<Node>> clones_;
    std::vector<uint32_t> created_ids_;
    std::vector<std::string> source_names_;
};

// ── Remove Component ────────────────────────────────────────────────────
// Keeps the removed component alive instead of destroying it, so undo puts
// the same object back, in its old place among the node's components.

class RemoveComponentCommand : public Command {
public:
    RemoveComponentCommand(Scene& scene, uint32_t node_id, ComponentTypeId type,
                           std::string display_name)
        : scene_{scene}, node_id_{node_id}, type_{type}
        , display_name_{std::move(display_name)} {}

    void execute() override {
        if (auto* node = scene_.find(node_id_)) {
            removed_ = node->components.extract(type_);
            scene_.mark_dirty();
        }
    }
    void undo() override {
        if (auto* node = scene_.find(node_id_)) {
            node->components.restore(std::move(removed_));
            scene_.mark_dirty();
        }
    }
    [[nodiscard]] std::string description() const override {
        return "Remove " + display_name_;
    }

private:
    Scene& scene_;
    uint32_t node_id_;
    ComponentTypeId type_;
    std::string display_name_;
    ComponentStorage::Detached removed_;
};

/// One finished edit of a component's fields, undone by putting back a copy
/// of the component as it was. The copies are made by each type's clone
/// rule, the same one Duplicate uses, so the component that comes back is
/// complete and shares its loaded model, materials and effects; what it was
/// doing at runtime (a playing sound, an animation's time) starts over.
class ComponentEditCommand : public Command {
public:
    /// `before` and `after` each hold one component of `type`: the state
    /// the edit started from and the state it ended in. The node already
    /// has `after` when the command is executed the first time.
    ComponentEditCommand(Scene& scene, uint32_t node_id, ComponentTypeId type,
                         ComponentStorage before, ComponentStorage after,
                         std::string description)
        : scene_{scene}, node_id_{node_id}, type_{type}
        , before_{std::move(before)}, after_{std::move(after)}
        , description_{std::move(description)} {}

    void execute() override {
        if (applied_) {
            applied_ = false;
            return;
        }
        apply(after_);
    }
    void undo() override { apply(before_); }
    [[nodiscard]] std::string description() const override { return description_; }

private:
    // Swaps the node's component for a fresh copy of `state`, in the same
    // place among its siblings so the inspector doesn't reorder.
    void apply(const ComponentStorage& state) {
        Node* node = scene_.find(node_id_);
        if (node == nullptr) return;
        ComponentStorage fresh;
        state.clone_one(type_, node->components, fresh);
        const size_t at = node->components.index_of(type_);
        ComponentStorage::Detached old = node->components.extract(type_);
        node->components.restore(fresh.extract(type_), at);
        scene_.mark_dirty();
    }

    Scene& scene_;
    uint32_t node_id_;
    ComponentTypeId type_;
    ComponentStorage before_;
    ComponentStorage after_;
    std::string description_;
    // The edit is already on the node when the command is first executed.
    bool applied_{true};
};

} // namespace fjell
