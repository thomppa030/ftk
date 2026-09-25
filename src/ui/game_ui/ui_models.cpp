#include "ui/game_ui/ui_models.hpp"

#include "core/log.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "ui/game_ui/ui_context.hpp"

#include <algorithm>

namespace fjell {

namespace {

/// The node's script of the type, or null.
const void* script_on(const Node& node, const std::string& type) {
    for (const auto& entry : node.scripts) {
        if (entry.instance && entry.type_name == type) return entry.instance.get();
    }
    return nullptr;
}

} // namespace

void UiModels::attach(const Scene& scene, const UiModelWorld& world, uint32_t self_node) {
    scene_ = &scene;
    world_ = &world;
    self_node_ = self_node;
}

void UiModels::add(const ModelDecl& decl, UiContext& scope, uint32_t placed) {
    Entry entry;
    entry.decl = decl;
    entry.scope = &scope;
    entry.placed = placed;
    entries_.push_back(std::move(entry));
}

void UiModels::clear() {
    entries_.clear();
    reported_.clear();
}

void UiModels::bind() {
    if (scene_ == nullptr || world_ == nullptr) return;
    for (auto& entry : entries_) {
        const auto type = world_->component_type(entry.decl.component);
        const void* component = type ? resolve(entry) : nullptr;
        if (!type && !entry.reported) {
            entry.reported = true;
            FJELL_CORE_WARN("{}:{}: model '{}' reads '{}', which is not a loaded script component",
                            entry.decl.source_file, entry.decl.source_line, entry.decl.name,
                            entry.decl.component);
        }
        entry.scope->set(entry.decl.name, component != nullptr
                                              ? UiValue(UiObject{type->name, type->fields, component})
                                              : UiValue());
    }
}

bool UiModels::bind_node(const UiContext& scope, std::string_view name, uint32_t node) {
    Entry* entry = find(scope, name);
    if (entry == nullptr) {
        report_once("models().bind(\"" + std::string(name) + "\"): the widget declares no <model name=\"" +
                    std::string(name) + "\">");
        return false;
    }
    entry->bound = node;
    entry->bound_generation = 0;
    return true;
}

const void* UiModels::component(const UiContext& scope, std::string_view name, std::string_view type) {
    Entry* entry = find(scope, name);
    if (entry == nullptr) {
        report_once("model<" + std::string(type) + ">(\"" + std::string(name) +
                    "\"): the widget declares no <model name=\"" + std::string(name) + "\">");
        return nullptr;
    }
    if (entry->decl.component != type) {
        report_once("model<" + std::string(type) + ">(\"" + std::string(name) + "\"): that Model reads '" +
                    entry->decl.component + "'");
        return nullptr;
    }
    if (scene_ == nullptr || world_ == nullptr) return nullptr;
    return resolve(*entry);
}

UiModels::Entry* UiModels::find(const UiContext& scope, std::string_view name) {
    for (auto& entry : entries_) {
        if (entry.scope == &scope && entry.decl.name == name) return &entry;
    }
    return nullptr;
}

const void* UiModels::resolve(Entry& entry) {
    if (entry.bound != 0) return on_chosen(entry.bound, entry.bound_generation, entry.decl.component);
    if (entry.placed != 0) return on_chosen(entry.placed, entry.placed_generation, entry.decl.component);
    if (entry.decl.from == ModelSource::self) {
        const Node* node = scene_->find(self_node_);
        return node != nullptr ? script_on(*node, entry.decl.component) : nullptr;
    }
    return find_local(entry);
}

const void* UiModels::on_chosen(uint32_t node, uint32_t& generation, const std::string& type) const {
    if (generation == 0) generation = scene_->generation_of(node);
    if (scene_->generation_of(node) != generation) return nullptr;
    const Node* found = scene_->find(node);
    return found != nullptr ? script_on(*found, type) : nullptr;
}

const void* UiModels::find_local(Entry& entry) {
    const Scene& scene = *scene_;
    const std::string& type = entry.decl.component;
    const uint64_t possession = world_->possession_revision();
    const bool same_scene = entry.searched && entry.searched_scene == scene.id();

    // What was found stays while its node stands, still carries the
    // component and has not changed hands.
    if (entry.node != 0 && same_scene && entry.searched_possession == possession &&
        scene.generation_of(entry.node) == entry.generation) {
        if (const Node* node = scene.find(entry.node)) {
            if (const void* component = script_on(*node, type)) return component;
        }
    }

    // Nothing was found, and nothing has joined, left or changed hands since.
    const uint64_t structure = scene.structure_revision();
    if (entry.node == 0 && same_scene && entry.searched_structure == structure &&
        entry.searched_possession == possession) {
        return nullptr;
    }

    ++searches_;
    entry.searched = true;
    entry.searched_scene = scene.id();
    entry.searched_structure = structure;
    entry.searched_possession = possession;
    entry.node = 0;
    for (const auto& node : scene.objects()) {
        const void* component = script_on(*node, type);
        if (component == nullptr || !world_->owned_by_local(node->id)) continue;
        entry.node = node->id;
        entry.generation = scene.generation_of(node->id);
        return component;
    }
    return nullptr;
}

void UiModels::report_once(const std::string& message) {
    if (std::ranges::find(reported_, message) != reported_.end()) return;
    reported_.push_back(message);
    FJELL_CORE_WARN("Game UI: {}", message);
}

} // namespace fjell
