#pragma once

#include "core/command.hpp"
#include "scene/scene.hpp"

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
        if (auto* obj = scene_.find(id_)) {
            obj->transform = new_transform_;
            scene_.mark_dirty();
        }
    }
    void undo() override {
        if (auto* obj = scene_.find(id_)) {
            obj->transform = old_transform_;
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
    std::unique_ptr<SceneNode> snapshot_;
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
    std::unique_ptr<SceneNode> snapshot_;
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

} // namespace fjell
