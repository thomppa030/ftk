// ui_widget.hpp — UiWidget base class for game UI widgets
#pragma once

#include "fjell/api.h"
#include "fjell/delegate.hpp"
#include "fjell/world.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fjell {

class UiContext;
class UiSubsystem;

/// Bridge for UiWidget to access its UiContext without including the full header.
/// The UiSubsystem sets this pointer after creating the widget.
class UiWidgetContext {
public:
    UiWidgetContext() = default;

    void set(const std::string& key, const std::string& value);
    [[nodiscard]] std::string get(const std::string& key) const;

    FJELL_SDK_EXPORT void bind_action(const std::string& name,
                     std::function<void()> callback);
    FJELL_SDK_EXPORT void bind_action(const std::string& name,
                     std::function<void(const std::vector<std::string>&)> callback);

private:
    friend class UiSubsystem;
    UiContext* ctx_{nullptr};
    // Connections are stored engine-side to avoid cross-library destruction
    std::vector<Connection>* connections_{nullptr};
};

/// Base class for UI widgets. A widget defines [[fj::bind]] fields and
/// [[fj::action]] methods that drive a .fjui/.fjss visual template.
/// Widgets are registered via FJELL_UI_WIDGET(Name) and referenced
/// in .fjwidget asset files.
class UiWidget {
public:
    virtual ~UiWidget() = default;

    virtual void begin_play() {}
    virtual void tick(float dt) { (void)dt; }
    virtual void end_play() {}

    [[nodiscard]] float delta_time() const { return world_->delta_time; }
    [[nodiscard]] float elapsed_time() const { return world_->elapsed_time; }
    [[nodiscard]] const Input& input() const { return world_->input(); }
    [[nodiscard]] World* world() { return world_; }
    [[nodiscard]] const World* world() const { return world_; }

    /// Access the UI context to set values, get values, and bind actions.
    [[nodiscard]] UiWidgetContext& ui() { return ui_context_; }

private:
    friend class UiSubsystem;
    World* world_{nullptr};
    UiWidgetContext ui_context_;
    uint32_t id_{0};
};

// Backward compat alias
using UiComponent = UiWidget;

} // namespace fjell
