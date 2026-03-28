#pragma once

#include "core/delegate.hpp"
#include "core/subsystem.hpp"
#include "scripting/fjell/ui_component.hpp"
#include "ui/game_ui/game_ui.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class ScriptManager;
class UiComponent;
class VulkanContext;
struct OwnedUiComponentType;
struct UiNode;

/// Manages runtime game UI instances. Finds nodes with UiWidgetComponent in
/// the scene on play start, creates a GameUi per node, and tears them down
/// on stop. Creates UiComponent controller instances and auto-syncs bind fields.
class UiSubsystem : public Subsystem {
public:
    UiSubsystem(VulkanContext& vulkan_context, GameUi& editor_game_ui);

    [[nodiscard]] std::string_view name() const override { return "GameUI"; }
    [[nodiscard]] TickPolicy tick_policy() const override { return TickPolicy::PLAY_ONLY; }

    void tick(const TickContext& ctx) override;
    void on_play_start(const TickContext& ctx) override;
    void on_play_stop(const TickContext& ctx) override;

    void set_script_manager(ScriptManager* sm) { script_manager_ = sm; }

    /// Access runtime UI instances (for render callback)
    struct ControllerInstance {
        std::unique_ptr<UiComponent> component;
        const OwnedUiComponentType* type;
        UiContext* widget_context;  // the widget's scoped context
        std::vector<Connection> connections;
    };

    struct UiInstance {
        uint32_t node_id;
        std::unique_ptr<GameUi> game_ui;
        std::vector<ControllerInstance> controllers;
    };
    [[nodiscard]] std::vector<UiInstance>& instances() { return instances_; }
    [[nodiscard]] const std::vector<UiInstance>& instances() const { return instances_; }

private:
    void collect_controllers(UiNode& node, UiInstance& instance);
    void sync_bind_fields(ControllerInstance& ctrl);

    VulkanContext& vulkan_context_;
    GameUi& editor_game_ui_;
    ScriptManager* script_manager_{nullptr};
    std::vector<UiInstance> instances_;
};

} // namespace fjell
