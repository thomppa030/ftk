#pragma once

#include "ui/game_ui/ui_value.hpp"
#include "ui/game_ui/widget_asset.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

class Scene;
class UiContext;
struct OwnedFieldDesc;

/// A script component type as a Model reads it.
struct UiComponentType {
    std::string_view name;                  ///< owned by the game module, like the fields
    std::span<const OwnedFieldDesc> fields;
};

/// What the UI asks the running game while it finds a Model's component.
/// The UI subsystem answers from the script manager and the network session.
class UiModelWorld {
public:
    virtual ~UiModelWorld() = default;

    /// The script component type by name; nothing when no loaded game module
    /// has one.
    [[nodiscard]] virtual std::optional<UiComponentType> component_type(const std::string& name) const = 0;

    /// Whether the local player controls the node. Offline, every node.
    [[nodiscard]] virtual bool owned_by_local(uint32_t node_id) const = 0;

    /// Moves whenever owned_by_local() may answer differently for some node.
    [[nodiscard]] virtual uint64_t possession_revision() const = 0;
};

/// The Models of one placed UI: the game components its widgets read in
/// place. A `local` Model's node is found by walking the scene once and then
/// kept; it is looked for again only when that node is gone, no longer
/// carries the component or changes hands, and, while nothing is found, only
/// once nodes have joined, left or changed hands since the last look. A
/// `self` Model reads the placement's own node, which is a lookup rather
/// than a walk. Nothing walks the scene every frame.
class UiModels {
public:
    /// A Model a widget declares, bound into that widget's scope. The scope
    /// belongs to the UI's tree: clear() before the tree is replaced.
    void add(const ModelDecl& decl, UiContext& scope);
    void clear() { entries_.clear(); }
    [[nodiscard]] bool empty() const { return entries_.empty(); }

    /// Every Model into its scope as the scene holds it now: the component as
    /// an object, or no value while none is found. The objects point into the
    /// components, so this runs after the frame's last destroy and before
    /// anything reads the UI. `self_node` is the node the UI is placed on.
    void bind(const Scene& scene, const UiModelWorld& world, uint32_t self_node);

    /// How many times a Model has walked the scene, for tests and profiling.
    [[nodiscard]] uint64_t searches() const { return searches_; }

private:
    struct Entry {
        ModelDecl decl;
        UiContext* scope{nullptr};
        uint32_t node{0};       ///< where the component was last found; 0 for nowhere
        uint32_t generation{0}; ///< that node's generation then
        // The scene and revisions the last walk saw.
        bool searched{false};
        uint64_t searched_scene{0};
        uint64_t searched_structure{0};
        uint64_t searched_possession{0};
        bool reported{false};   ///< an unknown component is reported once
    };

    /// The component of the local player's node, walking the scene only when
    /// what was found before no longer holds and something may have changed.
    [[nodiscard]] const void* find_local(const Scene& scene, const UiModelWorld& world, Entry& entry);

    std::vector<Entry> entries_;
    uint64_t searches_{0};
};

} // namespace fjell
