#include "ui/game_ui/ui_subsystem.hpp"
#include "core/base_path.hpp"
#include "core/frame_context.hpp"
#include "core/log.hpp"
#include "core/project.hpp"
#include "renderer/texture.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/game_ui_node.hpp"
#include "scene/scene.hpp"
#include "scripting/fjell/ui_component.hpp"
#include "scripting/script_manager.hpp"

#include <cstring>
#include <filesystem>

namespace fjell {

UiSubsystem::UiSubsystem(VulkanContext& vulkan_context, GameUi& editor_game_ui)
    : vulkan_context_{vulkan_context}, editor_game_ui_{editor_game_ui} {}

void UiSubsystem::on_play_start(const TickContext& ctx) {
    instances_.clear();

    for (const auto& obj : ctx.scene.objects()) {
        auto* gui_node = dynamic_cast<const GameUiNode*>(obj.get());
        if (!gui_node || gui_node->ui_path.empty()) continue;

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

        auto fjss_path = std::filesystem::path(fjui_path).replace_extension(".fjss").string();
        if (!std::filesystem::exists(fjss_path)) {
            fjss_path.clear();
        }

        auto game_ui = std::make_unique<GameUi>();
        vulkan_context_.init_game_ui_shared(*game_ui,
            editor_game_ui_.font(),
            vulkan_context_.swapchain_format());

        game_ui->load(fjui_path, fjss_path);
        game_ui->mark_dirty();

        UiInstance inst;
        inst.node_id = gui_node->id;
        inst.game_ui = std::move(game_ui);

        // Walk the resolved UI tree and create controllers for widget nodes
        if (script_manager_ && script_manager_->is_loaded()) {
            auto* root = inst.game_ui->root();
            if (root) {
                collect_controllers(*root, inst);
            }
        }

        // Call begin_play on all controllers
        for (auto& ctrl : inst.controllers) {
            ctrl.component->begin_play();
        }

        FJELL_CORE_INFO("UiSubsystem: loaded UI '{}' for node {} ({} controllers)",
                        gui_node->ui_path, gui_node->id, inst.controllers.size());

        instances_.push_back(std::move(inst));
    }
}

void UiSubsystem::collect_controllers(UiNode& node, UiInstance& instance) {
    if (node.is_widget && node.scoped_context && !node.widget_tag.empty()) {
        auto* ui_type = script_manager_->find_ui_type_by_tag(node.widget_tag);
        if (ui_type && ui_type->create) {
            ControllerInstance ctrl;
            ctrl.type = ui_type;
            ctrl.widget_context = node.scoped_context.get();
            ctrl.component.reset(ui_type->create());

            // Set up the component's world pointer (for input, delta_time, etc.)
            // The world is managed by ScriptManager — we share it
            // For now, controllers don't have world access until we wire it
            // TODO: wire world pointer from ScriptManager

            node.has_controller = true;

            FJELL_CORE_INFO("UiSubsystem: created controller '{}' for <{}>",
                            ui_type->name, node.widget_tag);

            instance.controllers.push_back(std::move(ctrl));
        }
    }

    for (auto& child : node.children) {
        collect_controllers(*child, instance);
    }
}

void UiSubsystem::sync_bind_fields(ControllerInstance& ctrl) {
    if (!ctrl.component || !ctrl.type || !ctrl.widget_context) return;

    auto* base = reinterpret_cast<const uint8_t*>(ctrl.component.get());

    for (const auto& field : ctrl.type->fields) {
        const void* ptr = base + field.offset;
        const char* key = field.name.c_str();

        switch (field.type) {
            case FJELL_FIELD_FLOAT:
                ctrl.widget_context->set_float(key, *static_cast<const float*>(ptr));
                break;
            case FJELL_FIELD_INT: {
                int val = *static_cast<const int*>(ptr);
                ctrl.widget_context->set_int(key, val);
                break;
            }
            case FJELL_FIELD_BOOL:
                ctrl.widget_context->set_bool(key, *static_cast<const bool*>(ptr));
                break;
            case FJELL_FIELD_STRING: {
                // std::string at offset — read the c_str
                const auto* str = reinterpret_cast<const std::string*>(ptr);
                ctrl.widget_context->set(key, *str);
                break;
            }
            default:
                // vec2/3/4, color, refs — not synced to UI context for now
                break;
        }
    }
}

void UiSubsystem::tick(const TickContext& ctx) {
    for (auto& inst : instances_) {
        for (auto& ctrl : inst.controllers) {
            if (!ctrl.component) continue;

            // Tick the controller
            ctrl.component->tick(ctx.dt);

            // Sync [[fj::bind]] fields → widget scoped context
            sync_bind_fields(ctrl);
        }
    }
}

void UiSubsystem::on_play_stop(const TickContext& /*ctx*/) {
    // Call end_play on all controllers
    for (auto& inst : instances_) {
        for (auto& ctrl : inst.controllers) {
            if (ctrl.component) {
                ctrl.component->end_play();
            }
            ctrl.connections.clear();
            ctrl.component.reset();
        }
    }

    vulkan_context_.wait_idle();

    for (auto& inst : instances_) {
        inst.game_ui->destroy();
    }
    instances_.clear();
}

} // namespace fjell
