// ui_widget.hpp — UiWidget base class for game UI widgets
#pragma once

#include "fjell/world.hpp"

#include <cstdint>

namespace fjell {

class UiSubsystem;

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

private:
    friend class UiSubsystem;
    World* world_{nullptr};
    uint32_t id_{0};
};

// Backward compat alias
using UiComponent = UiWidget;

} // namespace fjell
