#pragma once

#include "core/command.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "scripting/script_manager.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace fjell {

namespace detail {

// The node's script of `type_name`, or null.
inline ScriptEntry* find_script(Node& node, const std::string& type_name) {
    auto it = std::find_if(node.scripts.begin(), node.scripts.end(),
                           [&](const ScriptEntry& e) { return e.type_name == type_name; });
    return it != node.scripts.end() ? &*it : nullptr;
}

} // namespace detail

/// One finished edit of a script's fields, undone by setting them as they
/// were. The fields travel as the scene file stores them, so the command
/// holds nothing from the script library and survives a rebuild.
class ScriptEditCommand : public Command {
public:
    /// `before` and `after` are ScriptManager::fields_of() at the start and
    /// the end of the edit. The script already has `after` when the command
    /// is executed the first time.
    ScriptEditCommand(Scene& scene, ScriptManager& scripts, uint32_t node_id, std::string type_name,
                      nlohmann::json before, nlohmann::json after)
        : scene_{scene}, scripts_{scripts}, node_id_{node_id}, type_name_{std::move(type_name)}
        , before_(std::move(before)), after_(std::move(after)) {}

    void execute() override {
        if (applied_) {
            applied_ = false;
            return;
        }
        apply(after_);
    }
    void undo() override { apply(before_); }
    [[nodiscard]] std::string description() const override { return "Edit " + type_name_; }

private:
    void apply(const nlohmann::json& fields) {
        Node* node = scene_.find(node_id_);
        ScriptEntry* entry = node != nullptr ? detail::find_script(*node, type_name_) : nullptr;
        if (entry == nullptr) return;
        scripts_.set_fields(*entry, fields);
        scene_.mark_dirty();
    }

    Scene& scene_;
    ScriptManager& scripts_;
    uint32_t node_id_;
    std::string type_name_;
    nlohmann::json before_;
    nlohmann::json after_;
    // The edit is already on the node when the command is first executed.
    bool applied_{true};
};

/// Takes a script off a node. Undo puts it back in the same place with the
/// fields it had. Only the fields are kept, never the instance: a rebuild
/// while the command waits in the history unloads the code an instance
/// would need.
class RemoveScriptCommand : public Command {
public:
    RemoveScriptCommand(Scene& scene, ScriptManager& scripts, uint32_t node_id, std::string type_name)
        : scene_{scene}, scripts_{scripts}, node_id_{node_id}, type_name_{std::move(type_name)} {}

    void execute() override {
        Node* node = scene_.find(node_id_);
        ScriptEntry* entry = node != nullptr ? detail::find_script(*node, type_name_) : nullptr;
        if (entry == nullptr) return;
        fields_ = scripts_.fields_of(*entry);
        index_ = static_cast<std::size_t>(entry - node->scripts.data());
        node->scripts.erase(node->scripts.begin() + static_cast<std::ptrdiff_t>(index_));
        scene_.mark_dirty();
    }
    void undo() override {
        Node* node = scene_.find(node_id_);
        if (node == nullptr) return;
        ScriptEntry entry;
        entry.type_name = type_name_;
        entry.serialized = fields_;
        if (Node3D* spatial = node->as_spatial()) scripts_.instantiate(*spatial, entry);
        const std::size_t at = std::min(index_, node->scripts.size());
        node->scripts.insert(node->scripts.begin() + static_cast<std::ptrdiff_t>(at), std::move(entry));
        scene_.mark_dirty();
    }
    [[nodiscard]] std::string description() const override { return "Remove " + type_name_; }

private:
    Scene& scene_;
    ScriptManager& scripts_;
    uint32_t node_id_;
    std::string type_name_;
    nlohmann::json fields_;
    std::size_t index_{0};
};

/// Adds a script to a node. Undo takes it off keeping its fields, and redo
/// puts it back with them, so values set in between survive the round trip.
class AddScriptCommand : public Command {
public:
    AddScriptCommand(Scene& scene, ScriptManager& scripts, uint32_t node_id, std::string type_name)
        : scene_{scene}, scripts_{scripts}, node_id_{node_id}, type_name_{std::move(type_name)} {}

    void execute() override {
        Node* node = scene_.find(node_id_);
        Node3D* spatial = node != nullptr ? node->as_spatial() : nullptr;
        if (spatial == nullptr) return;
        ScriptEntry entry;
        entry.type_name = type_name_;
        entry.serialized = fields_;
        scripts_.instantiate(*spatial, entry);
        spatial->scripts.push_back(std::move(entry));
        scene_.mark_dirty();
    }
    void undo() override {
        Node* node = scene_.find(node_id_);
        ScriptEntry* entry = node != nullptr ? detail::find_script(*node, type_name_) : nullptr;
        if (entry == nullptr) return;
        fields_ = scripts_.fields_of(*entry);
        node->scripts.erase(node->scripts.begin() + (entry - node->scripts.data()));
        scene_.mark_dirty();
    }
    [[nodiscard]] std::string description() const override { return "Add " + type_name_; }

private:
    Scene& scene_;
    ScriptManager& scripts_;
    uint32_t node_id_;
    std::string type_name_;
    nlohmann::json fields_;
};

} // namespace fjell
