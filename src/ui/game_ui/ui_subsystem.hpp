#pragma once

#include "core/delegate.hpp"
#include "core/subsystem.hpp"
#include "scripting/fjell/ui_widget.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/ui_renderer.hpp"
#include "ui/game_ui/ui_widget_component.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

class Scene;
class ScriptManager;
class VulkanContext;
struct OwnedUiWidgetType;
struct UiNode;

/// Manages runtime game UI instances. Finds nodes with UiWidgetComponent in
/// the scene on play start, creates a GameUi per node, and tears them down
/// on stop. Creates UiWidget instances and auto-syncs bind fields.
class UiSubsystem : public Subsystem {
public:
    UiSubsystem(VulkanContext& vulkan_context, GameUi& editor_game_ui);

    [[nodiscard]] std::string_view name() const override { return "GameUI"; }
    [[nodiscard]] TickPolicy tick_policy() const override { return TickPolicy::PLAY_ONLY; }

    void tick(const TickContext& ctx) override;
    void on_play_start(const TickContext& ctx) override;
    void on_play_stop(const TickContext& ctx) override;

    void set_script_manager(ScriptManager* sm) { script_manager_ = sm; }

    struct WidgetInstance {
        std::unique_ptr<UiWidget> widget;
        const OwnedUiWidgetType* type;
        UiContext* widget_context;
        std::vector<Connection> connections;
    };

    struct UiInstance {
        uint32_t node_id;
        std::unique_ptr<GameUi> game_ui;
        std::vector<WidgetInstance> widgets;
        UiWidgetComponent::RenderMode render_mode{UiWidgetComponent::RenderMode::screen_space};
    };
    [[nodiscard]] std::vector<UiInstance>& instances() { return instances_; }
    [[nodiscard]] const std::vector<UiInstance>& instances() const { return instances_; }

    struct WorldUiRecord {
        GameUi* game_ui;
        UiWorldPushConstants push;
    };
    [[nodiscard]] const std::vector<WorldUiRecord>& world_records() const { return world_records_; }

    void set_camera(const glm::vec3& pos, const glm::mat4& view, const glm::mat4& proj);

private:
    void collect_widgets(UiNode& node, UiInstance& instance);
    void sync_bind_fields(WidgetInstance& wi);
    void resolve_widget_bindings(Scene& scene);

    VulkanContext& vulkan_context_;
    GameUi& editor_game_ui_;
    ScriptManager* script_manager_{nullptr};
    std::vector<UiInstance> instances_;
    std::vector<WorldUiRecord> world_records_;

    glm::vec3 camera_position_{0.0f};
    glm::mat4 view_matrix_{1.0f};
    glm::mat4 projection_matrix_{1.0f};
};

} // namespace fjell
