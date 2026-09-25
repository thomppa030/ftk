// ui_widget.hpp — UiWidget base class for game UI widgets
#pragma once

#include "fjell/api.h"
#include "fjell/delegate.hpp"
#include "fjell/world.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fjell {

class ScriptComponent;
class UiContext;
class UiModels;
class UiSubsystem;

/// The widget's connection to its `.fjui` template — the data it displays and
/// the actions its markup can invoke. Reached through `UiWidget::ui()`.
class UiWidgetContext {
public:
    UiWidgetContext() = default;

    /// Publish a value for the template to display. A `{key}` placeholder in
    /// the `.fjui` markup picks it up, and `if="key"` / `class:x="key==value"`
    /// test against it. Setting an unchanged value is cheap — safe from `tick()`.
    /// @param key   Matches the name used in the markup.
    /// @param value Everything is stringly-typed here; format numbers yourself.
    FJELL_SDK_EXPORT void set(const std::string& key, const std::string& value);

    /// Read back a value previously published with `set()`.
    /// @return The value, or "" if the key was never set.
    [[nodiscard]] FJELL_SDK_EXPORT std::string get(const std::string& key) const;

    /// Size the list a `foreach="key"` element repeats over: the element is
    /// stamped out once per item. Items keep their values when the list
    /// grows or shrinks at its end, and resizing to the current size does
    /// nothing, so this is cheap to call from `tick()`.
    /// @param key   The list's name, as written in `foreach="key"`.
    /// @param count How many items; 0 shows no repeats.
    FJELL_SDK_EXPORT void set_list_size(const std::string& key, std::size_t count);

    /// Publish a value on one item of a list. Inside that item's repeat,
    /// `{field}` reads it; a field the item lacks falls back to the widget's
    /// own values. An unchanged value is cheap, so this is safe from `tick()`.
    /// @param key   The list's name, as written in `foreach="key"`.
    /// @param index The item, from 0. Ignored past the end of the list, or
    ///              when `set_list_size()` never created it.
    /// @param field The name the markup reads, as in `{field}`.
    /// @param value Stringly-typed like `set()`; format numbers yourself.
    FJELL_SDK_EXPORT void set_item(const std::string& key, std::size_t index,
                                   const std::string& field, const std::string& value);

    /// Bind a callback to an `on:click="name"` in the markup.
    /// @param name     Action name as spelled in the `.fjui` file.
    /// @param callback Invoked on click. Capturing `this` is fine — the
    ///                 binding is released with the widget.
    FJELL_SDK_EXPORT void bind_action(const std::string& name,
                     std::function<void()> callback);

    /// Bind a callback that receives the arguments from the markup, for
    /// parameterized actions like `on:click="select(3)"`.
    /// @param name     Action name as spelled in the `.fjui` file.
    /// @param callback Receives the arguments as strings, in written order.
    FJELL_SDK_EXPORT void bind_action(const std::string& name,
                     std::function<void(const std::vector<std::string>&)> callback);

private:
    friend class UiSubsystem;
    UiContext* ctx_{nullptr};
    // Connections are stored engine-side to avoid cross-library destruction
    std::vector<Connection>* connections_{nullptr};
};

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
    /// @param node_id The node, as `Node3D::id()` gives it.
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
/// Derive, add `FJELL_UI_WIDGET(T)`, and reference the type from a `.fjwidget`
/// asset. `[[fj::bind]]` fields publish themselves to the template
/// automatically; `[[fj::action]]` methods become callable from `on:click`.
///
/// This is the in-game UI system, separate from the ImGui editor chrome.
///
/// @code
/// class HealthBar : public fjell::UiWidget {
///     FJELL_UI_WIDGET(HealthBar)
/// public:
///     [[fj::bind]] int hp = 100;      // reachable as {hp} in the .fjui
///
///     [[fj::action]] void on_close() { visible_ = false; }
/// };
/// @endcode
class UiWidget {
public:
    virtual ~UiWidget() = default;

    /// Called once when the widget is created and its template is loaded.
    /// Bind actions and publish initial values here.
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

    /// The template bridge — publish values with `ui().set()` and wire
    /// markup actions with `ui().bind_action()`.
    [[nodiscard]] UiWidgetContext& ui() { return ui_context_; }

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
    UiWidgetContext ui_context_;
    UiWidgetModels models_;
    uint32_t id_{0};
};

// Backward compat alias
using UiComponent = UiWidget;

} // namespace fjell
