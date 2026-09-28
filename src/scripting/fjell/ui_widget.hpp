// ui_widget.hpp — UiWidget base class for game UI widgets
#pragma once

#include "fjell/api.h"
#include "fjell/world.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

class ScriptComponent;
class UiContext;
class UiModels;
class UiSubsystem;

/// The widget's Models as its controller sees them: which node each reads,
/// for choices made at runtime. A Model is declared in the widget's markup,
/// `<model name="target" component="EnemyStats"/>`, and reached through
/// `UiWidget::models()`.
class UiWidgetModels {
public:
    /// Read the Model `name` from this node instead of by its `from` rule,
    /// until `clear()` or another `bind()`: a target frame showing whatever
    /// the player has targeted. A node that is destroyed, or does not carry
    /// the Model's component, leaves it empty.
    /// @param name    As declared in the widget's `<model name>`. A name the
    ///                widget does not declare is reported once and ignored.
    /// @param node_id The node, as `ScriptComponent::id()` or `Ref::node_id()` gives it.
    FJELL_SDK_EXPORT void bind(const std::string& name, uint32_t node_id);

    /// Return the Model `name` to its `from` rule.
    FJELL_SDK_EXPORT void clear(const std::string& name);

private:
    friend class UiSubsystem;
    friend class UiWidget;
    /// The component the Model reads now if it is a `type_name`; null
    /// otherwise.
    [[nodiscard]] FJELL_SDK_EXPORT const ScriptComponent* component(const std::string& name,
                                                                     const char* type_name) const;
    UiModels* models_{nullptr};
    const UiContext* scope_{nullptr};
};

/// Base class for game UI — HUDs, menus, inventories. A widget supplies the
/// data and behavior; a `.fjui` template supplies the markup and a `.fjss`
/// stylesheet the look.
///
/// Derive, add `FJELL_UI_WIDGET(T)`, and name the type in a `.fjwidget`'s
/// `class`. The markup reads the widget where it is, by name: its
/// `[[fj::model]]` structs and its `[[fj::state]]` fields and getters.
/// `[[fj::prop]]` fields are filled from the widget's props of their names,
/// and `[[fj::action]]` methods are called from `on:click`.
///
/// This is the in-game UI system, separate from the ImGui editor chrome.
///
/// @code
/// struct InventoryModel {
///     std::vector<ItemSlot> slots;
///     int selected = 0;
/// };
///
/// class inventory_controller : public fjell::UiWidget {
///     FJELL_UI_WIDGET(inventory_controller)
/// public:
///     [[fj::model]] InventoryModel inventory; // {inventory.selected} in markup
///     [[fj::prop]] int columns = 4;           // <prop name="columns" type="int"/>
///     [[fj::state]] bool open = false;        // if="open"
///
///     [[fj::action]] void select(int slot) { inventory.selected = slot; }
/// };
/// @endcode
class UiWidget {
public:
    virtual ~UiWidget() = default;

    /// Called once when the widget is created and its template is loaded,
    /// its props already bound. Set up its Models here.
    virtual void begin_play() {}

    /// Called every frame while the widget exists.
    /// @param dt Seconds since the previous frame, scaled by `Time::set_scale()`.
    ///           Unlike gameplay, UI keeps ticking while the game is paused.
    virtual void tick(float dt) { (void)dt; }

    /// Called when the widget is torn down or scripts hot-reload.
    virtual void end_play() {}

    /// Seconds since the previous frame — the same value passed to `tick()`.
    [[nodiscard]] float delta_time() const { return world_->delta_time; }

    /// Seconds since play started.
    [[nodiscard]] float elapsed_time() const { return world_->elapsed_time; }

    /// Raw keyboard and mouse state. Clicks on UI elements arrive through
    /// `on:click` actions instead — use this only for shortcuts the markup
    /// cannot express.
    [[nodiscard]] const Input& input() const { return world_->input(); }

    /// Shared per-frame runtime state (input, physics, timing).
    [[nodiscard]] World* world() { return world_; }
    [[nodiscard]] const World* world() const { return world_; }

    /// The widget's Models: bind one to a node chosen at runtime.
    [[nodiscard]] UiWidgetModels& models() { return models_; }

    /// The component the Model `name` reads now, the same instance the markup
    /// shows. Read-only, as Models are. Null while the Model finds nothing,
    /// and, reported once, when the widget declares no Model `name` or that
    /// Model reads another type.
    /// @code
    /// if (const auto* stats = model<PlayerStats>("player")) calm_ = stats->full();
    /// @endcode
    template <class T>
    [[nodiscard]] const T* model(const std::string& name) const {
        return static_cast<const T*>(models_.component(name, T::type_name));
    }

private:
    friend class UiSubsystem;
    World* world_{nullptr};
    UiWidgetModels models_;
    uint32_t id_{0};
};

// Backward compat alias
using UiComponent = UiWidget;

} // namespace fjell
