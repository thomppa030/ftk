#pragma once

#include "core/subsystem.hpp"
#include "ui/game_ui/game_ui.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class VulkanContext;

/// Manages runtime game UI instances. Finds GameUiNode nodes in the scene
/// on play start, creates a GameUi per node, and tears them down on stop.
class UiSubsystem : public Subsystem {
public:
    UiSubsystem(VulkanContext& vulkan_context, GameUi& editor_game_ui);

    [[nodiscard]] std::string_view name() const override { return "GameUI"; }
    [[nodiscard]] TickPolicy tick_policy() const override { return TickPolicy::PLAY_ONLY; }

    void tick(const TickContext& ctx) override;
    void on_play_start(const TickContext& ctx) override;
    void on_play_stop(const TickContext& ctx) override;

    /// Access runtime UI instances (for render callback)
    struct UiInstance {
        uint32_t node_id;
        std::unique_ptr<GameUi> game_ui;
    };
    [[nodiscard]] std::vector<UiInstance>& instances() { return instances_; }
    [[nodiscard]] const std::vector<UiInstance>& instances() const { return instances_; }

private:
    VulkanContext& vulkan_context_;
    GameUi& editor_game_ui_;  // for sharing the font atlas
    std::vector<UiInstance> instances_;
};

} // namespace fjell
