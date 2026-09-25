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
/// `self` Model reads the placement's own node, and one a Controller has bound
/// reads the node it was given: lookups rather than walks. Nothing walks the
/// scene every frame.
class UiModels {
public:
    /// Where the Models are found: the scene, the game, and the node the UI
    /// is placed on. Both must outlive the Models, or be attached anew.
    void attach(const Scene& scene, const UiModelWorld& world, uint32_t self_node);

    /// A Model a widget declares, bound into that widget's scope. The scope
    /// belongs to the UI's tree: clear() before the tree is replaced.
    void add(const ModelDecl& decl, UiContext& scope);
    void clear();
    [[nodiscard]] bool empty() const { return entries_.empty(); }

    /// Every Model into its scope as the scene holds it now: the component as
    /// an object, or no value while none is found. The objects point into the
    /// components, so this runs after the frame's last destroy and before
    /// anything reads the UI.
    void bind();

    /// Read the Model `name`, declared by the widget whose scope is `scope`,
    /// from `node` rather than by its `from` rule; node 0 returns it to its
    /// rule. A destroyed node leaves the Model empty until it is bound again.
    /// False, reported once, when that widget declares no such Model.
    bool bind_node(const UiContext& scope, std::string_view name, uint32_t node);

    /// The component the Model reads now, found as bind() would find it:
    /// null while there is none, and, reported once, when the Model is not
    /// of `type` or the widget declares no Model `name`.
    [[nodiscard]] const void* component(const UiContext& scope, std::string_view name, std::string_view type);

    /// How many times a Model has walked the scene, for tests and profiling.
    [[nodiscard]] uint64_t searches() const { return searches_; }

private:
    struct Entry {
        ModelDecl decl;
        UiContext* scope{nullptr};
        uint32_t node{0};       ///< where the rule last found the component; 0 for nowhere
        uint32_t generation{0}; ///< that node's generation then
        // The scene and revisions the rule's last walk saw.
        bool searched{false};
        uint64_t searched_scene{0};
        uint64_t searched_structure{0};
        uint64_t searched_possession{0};
        uint32_t bound{0};            ///< a node a Controller chose; 0 follows the rule
        uint32_t bound_generation{0}; ///< its generation when first looked up; 0 not yet
        bool reported{false};   ///< an unknown component is reported once
    };

    [[nodiscard]] Entry* find(const UiContext& scope, std::string_view name);
    /// The component the entry reads now, or null.
    [[nodiscard]] const void* resolve(Entry& entry);
    /// The component of the local player's node, walking the scene only when
    /// what was found before no longer holds and something may have changed.
    [[nodiscard]] const void* find_local(Entry& entry);
    void report_once(const std::string& message);

    const Scene* scene_{nullptr};
    const UiModelWorld* world_{nullptr};
    uint32_t self_node_{0};
    std::vector<Entry> entries_;
    std::vector<std::string> reported_;
    uint64_t searches_{0};
};

} // namespace fjell
