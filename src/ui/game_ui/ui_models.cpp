#include "ui/game_ui/ui_models.hpp"

#include "core/log.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "ui/game_ui/ui_context.hpp"

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

void UiModels::add(const ModelDecl& decl, UiContext& scope) {
    Entry entry;
    entry.decl = decl;
    entry.scope = &scope;
    entries_.push_back(std::move(entry));
}

void UiModels::bind(const Scene& scene, const UiModelWorld& world, uint32_t self_node) {
    for (auto& entry : entries_) {
        const auto type = world.component_type(entry.decl.component);
        if (!type) {
            if (!entry.reported) {
                entry.reported = true;
                FJELL_CORE_WARN("{}:{}: model '{}' reads '{}', which is not a loaded script component",
                                entry.decl.source_file, entry.decl.source_line, entry.decl.name,
                                entry.decl.component);
            }
            entry.scope->set(entry.decl.name, UiValue());
            continue;
        }

        const void* component = nullptr;
        if (entry.decl.from == ModelSource::self) {
            const Node* node = scene.find(self_node);
            component = node != nullptr ? script_on(*node, entry.decl.component) : nullptr;
        } else {
            component = find_local(scene, world, entry);
        }
        entry.scope->set(entry.decl.name, component != nullptr
                                              ? UiValue(UiObject{type->name, type->fields, component})
                                              : UiValue());
    }
}

const void* UiModels::find_local(const Scene& scene, const UiModelWorld& world, Entry& entry) {
    const std::string& type = entry.decl.component;
    const uint64_t possession = world.possession_revision();
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
        if (component == nullptr || !world.owned_by_local(node->id)) continue;
        entry.node = node->id;
        entry.generation = scene.generation_of(node->id);
        return component;
    }
    return nullptr;
}

} // namespace fjell
