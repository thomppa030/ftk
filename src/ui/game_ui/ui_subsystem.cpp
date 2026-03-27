#include "ui/game_ui/ui_subsystem.hpp"
#include "core/base_path.hpp"
#include "core/frame_context.hpp"
#include "core/log.hpp"
#include "core/project.hpp"
#include "renderer/texture.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/game_ui_node.hpp"
#include "scene/scene.hpp"

#include <filesystem>

namespace fjell {

UiSubsystem::UiSubsystem(VulkanContext& vulkan_context, GameUi& editor_game_ui)
    : vulkan_context_{vulkan_context}, editor_game_ui_{editor_game_ui} {}

void UiSubsystem::on_play_start(const TickContext& ctx) {
    instances_.clear();

    // Find all GameUiNode nodes in the scene
    for (const auto& obj : ctx.scene.objects()) {
        auto* gui_node = dynamic_cast<const GameUiNode*>(obj.get());
        if (!gui_node || gui_node->ui_path.empty()) continue;

        // Resolve the .fjui path relative to project root
        std::string fjui_path;
        if (ctx.project) {
            fjui_path = ctx.project->resolve(gui_node->ui_path);
        } else {
            fjui_path = base_path::resolve(gui_node->ui_path);
        }

        if (!std::filesystem::exists(fjui_path)) {
            FJELL_CORE_WARN("UiSubsystem: .fjui not found: {}", fjui_path);
            continue;
        }

        // Derive .fjss path
        auto fjss_path = std::filesystem::path(fjui_path).replace_extension(".fjss").string();
        if (!std::filesystem::exists(fjss_path)) {
            fjss_path.clear();
        }

        // Create GameUi instance with shared font from the editor's GameUi
        auto game_ui = std::make_unique<GameUi>();
        vulkan_context_.init_game_ui_shared(*game_ui,
            editor_game_ui_.font(),
            vulkan_context_.swapchain_format());

        game_ui->load(fjui_path, fjss_path);
        game_ui->mark_dirty();

        FJELL_CORE_INFO("UiSubsystem: loaded UI '{}' for node {}",
                        gui_node->ui_path, gui_node->id);

        UiInstance inst;
        inst.node_id = gui_node->id;
        inst.game_ui = std::move(game_ui);
        instances_.push_back(std::move(inst));
    }
}

void UiSubsystem::tick(const TickContext& /*ctx*/) {
    // Step 9 future: tick UiComponent controllers, auto-sync [[fj::bind]] fields
}

void UiSubsystem::on_play_stop(const TickContext& /*ctx*/) {
    // Wait for GPU to finish before destroying Vulkan resources (SSBOs, pipelines)
    vulkan_context_.wait_idle();

    for (auto& inst : instances_) {
        inst.game_ui->destroy();
    }
    instances_.clear();
}

} // namespace fjell
