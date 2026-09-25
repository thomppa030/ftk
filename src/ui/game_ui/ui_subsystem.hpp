#pragma once

#include "core/delegate.hpp"
#include "core/subsystem.hpp"
#include "scripting/fjell/ui_widget.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/ui_models.hpp"
#include "ui/game_ui/ui_renderer.hpp"
#include "ui/game_ui/ui_widget_component.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

class NetManager;
class Scene;
class ScriptManager;
class VulkanContext;
struct OwnedUiWidgetType;
struct UiNode;
struct WidgetAsset;

/// Manages runtime game UI instances. Finds nodes with UiWidgetComponent in
/// the scene on play start, creates a GameUi per node, and tears them down
/// on stop. Creates UiWidget instances and auto-syncs bind fields.
class UiSubsystem : public Subsystem {
public:
    UiSubsystem(VulkanContext& vulkan_context, ScriptManager& script_manager);

    void set_resource_manager(ResourceManager* rm) { resource_manager_ = rm; }
    /// Who controls which node, for `from="local"` Models. Without one,
    /// the local player controls every node.
    void set_net_manager(const NetManager* net) { net_manager_ = net; }

    [[nodiscard]] std::string_view name() const override { return "GameUI"; }

    void tick(const TickContext& ctx) override;
    void on_play_start(const TickContext& ctx) override;
    void on_play_stop(const TickContext& ctx) override;

    // Rebuilds world_records_ from each instance's node transform. Split out
    // of tick() because tick() runs in play_phase_0_, before scripts and
    // physics have moved anything this frame — Scene::compute_world_matrices
    // settles world_matrix_ afterward. Called from that point instead, so
    // world-space UI reads the same transform the mesh it's attached to
    // renders with this frame, not last frame's.
    void update_world_records(const TickContext& ctx);

    // Puts each instance's Models into their scopes as the scene holds them
    // now. A Model points into its component, so this runs after the last
    // destroy of the frame and before anything reads the UI (drawing and
    // input both happen at record time).
    void bind_models(const TickContext& ctx);

    struct WidgetInstance {
        std::unique_ptr<UiWidget> widget;
        const OwnedUiWidgetType* type;
        UiContext* widget_context;
        std::vector<Connection> connections;
    };

    struct UiInstance {
        uint32_t node_id;
        std::string widget_path; // absolute .fjwidget path the instance is built from
        // Classes the placing component adds to the root node, applied on
        // every load so a reload keeps the placement's chosen variant.
        std::vector<std::string> style_classes;
        std::unique_ptr<GameUi> game_ui;
        std::vector<WidgetInstance> widgets;
        UiWidgetComponent::RenderMode render_mode{UiWidgetComponent::RenderMode::screen_space};
        uint64_t tick_counter{0};
        // World-space only: this widget's mvp from the previous frame, so the
        // velocity attachment reflects its own motion rather than whatever
        // was already under its pixels. Identity until the first frame it's
        // actually recorded, which briefly reads as "didn't move" — a single
        // frame of no smear rather than a wrong one.
        glm::mat4 prev_mvp{1.0f};
    };
    [[nodiscard]] std::vector<UiInstance>& instances() { return instances_; }
    [[nodiscard]] const std::vector<UiInstance>& instances() const { return instances_; }

    struct WorldUiRecord {
        GameUi* game_ui;
        UiWorldPushConstants push;
        glm::mat4 prev_mvp{1.0f};
    };
    [[nodiscard]] const std::vector<WorldUiRecord>& world_records() const { return world_records_; }

    void set_camera(const glm::vec3& pos, const glm::mat4& view, const glm::mat4& proj);

private:
    // Builds (or rebuilds) an instance's UI tree and controllers from a
    // parsed widget asset. Safe to call on a live instance: the previous
    // controllers are torn down first.
    void apply_widget(UiInstance& inst, const WidgetAsset& widget_asset);
    void create_controllers(UiInstance& inst, const WidgetAsset& widget_asset);
    // Every <model> in the instance's tree, each bound into the scope of the
    // widget that declares it.
    void collect_models(UiInstance& inst, const WidgetAsset& widget_asset);
    // A controller's [[fj::model]] fields into its scope, by field name.
    static void bind_controller_models(WidgetInstance& wi);
    void destroy_controllers(UiInstance& inst);
    // Re-reads the instance's .fjwidget from disk after it changed. A file
    // that fails to parse leaves the instance showing its last good tree.
    void reload_instance(UiInstance& inst);
    void collect_widgets(UiNode& node, UiInstance& instance);
    void sync_bind_fields(WidgetInstance& wi);
    void resolve_widget_bindings(Scene& scene);
    void validate_widget_actions(const UiNode& node, const OwnedUiWidgetType* type);

    VulkanContext& vulkan_context_;
    ScriptManager* script_manager_{nullptr};
    ResourceManager* resource_manager_{nullptr};
    const NetManager* net_manager_{nullptr};
    // The scene and game the instances' Models are found in, for one play
    // session.
    const Scene* scene_{nullptr};
    std::unique_ptr<UiModelWorld> model_world_;
    std::vector<UiInstance> instances_;
    std::vector<WorldUiRecord> world_records_;

    glm::vec3 camera_position_{0.0f};
    glm::mat4 view_matrix_{1.0f};
    glm::mat4 projection_matrix_{1.0f};
};

} // namespace fjell
