#include "ui/game_ui/ui_subsystem.hpp"
#include "core/base_path.hpp"
#include "core/frame_context.hpp"
#include "core/log.hpp"
#include "core/project.hpp"
#include "renderer/texture.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "scripting/fjell/api.h"
#include "ui/game_ui/ui_widget_component.hpp"
#include "ui/game_ui/widget_asset.hpp"
#include "scripting/script_manager.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>

namespace fjell {

UiSubsystem::UiSubsystem(VulkanContext& vulkan_context, GameUi& editor_game_ui)
    : vulkan_context_{vulkan_context}, editor_game_ui_{editor_game_ui} {}

void UiSubsystem::on_play_start(const TickContext& ctx) {
    instances_.clear();
    world_records_.clear();

    for (const auto& obj : ctx.scene.objects()) {
        const auto* wc = obj->components.get<UiWidgetComponent>();
        if (!wc || wc->widget_path.empty()) continue;

        // Load the .fjwidget asset
        std::string widget_resolved;
        if (ctx.project) {
            widget_resolved = ctx.project->resolve(wc->widget_path);
        } else {
            widget_resolved = base_path::resolve(wc->widget_path);
        }

        auto widget_result = load_widget_asset(widget_resolved);
        if (!widget_result) {
            FJELL_CORE_WARN("UiSubsystem: {}", widget_result.error());
            continue;
        }
        const auto& widget_asset = *widget_result;

        // Resolve .fjui path
        std::string fjui_path;
        if (ctx.project) {
            fjui_path = ctx.project->resolve(widget_asset.fjui_path);
        } else {
            fjui_path = base_path::resolve(widget_asset.fjui_path);
        }

        if (!std::filesystem::exists(fjui_path)) {
            FJELL_CORE_WARN("UiSubsystem: .fjui not found: {}", fjui_path);
            continue;
        }

        // Resolve .fjss path
        std::string fjss_path;
        if (!widget_asset.fjss_path.empty()) {
            if (ctx.project) {
                fjss_path = ctx.project->resolve(widget_asset.fjss_path);
            } else {
                fjss_path = base_path::resolve(widget_asset.fjss_path);
            }
        } else {
            // Default: same name as .fjui with .fjss extension
            fjss_path = std::filesystem::path(fjui_path).replace_extension(".fjss").string();
        }
        if (!std::filesystem::exists(fjss_path)) {
            fjss_path.clear();
        }

        auto render_mode = wc->render_mode;

        auto game_ui = std::make_unique<GameUi>();
        if (render_mode == UiWidgetComponent::RenderMode::world_space) {
            vulkan_context_.init_game_ui_world(*game_ui, editor_game_ui_.font());
        } else {
            vulkan_context_.init_game_ui_shared(*game_ui,
                editor_game_ui_.font(),
                vulkan_context_.swapchain_format());
        }

        game_ui->load(fjui_path, fjss_path);
        game_ui->mark_dirty();

        UiInstance inst;
        inst.node_id = obj->id;
        inst.game_ui = std::move(game_ui);
        inst.render_mode = render_mode;

        if (script_manager_ && script_manager_->is_loaded()) {
            // Create the top-level widget from .fjwidget class name
            if (!widget_asset.class_name.empty()) {
                auto* wt = script_manager_->find_ui_widget_type(widget_asset.class_name);
                if (wt && wt->create) {
                    WidgetInstance wi;
                    wi.type = wt;
                    wi.widget_context = &inst.game_ui->context();
                    wi.widget.reset(wt->create());
                    inst.widgets.push_back(std::move(wi));
                    FJELL_CORE_INFO("UiSubsystem: created widget '{}'", widget_asset.class_name);
                }
            }

            // Also walk the resolved UI tree for sub-widgets (embedded via tags)
            auto* root = inst.game_ui->root();
            if (root) {
                collect_widgets(*root, inst);
            }
        }

        for (auto& wi : inst.widgets) {
            wi.widget->begin_play();
        }

        FJELL_CORE_INFO("UiSubsystem: loaded widget '{}' for node {} ({} sub-widgets)",
                        wc->widget_path, obj->id, inst.widgets.size());

        instances_.push_back(std::move(inst));
    }

    // Resolve [[fj::bind]] UiWidget* pointers on ScriptComponents
    resolve_widget_bindings(ctx.scene);
}

void UiSubsystem::resolve_widget_bindings(Scene& scene) {
    if (!script_manager_ || !script_manager_->is_loaded()) return;

    // Build lookup: node_id → UiWidget pointer
    std::unordered_map<uint32_t, UiWidget*> node_to_widget;
    for (auto& inst : instances_) {
        if (!inst.widgets.empty()) {
            node_to_widget[inst.node_id] = inst.widgets[0].widget.get();
        }
    }

    for (const auto& obj : scene.objects()) {
        if (obj->scripts.empty()) continue;

        for (auto& script_entry : obj->scripts) {
            if (!script_entry.instance) continue;

            auto* script_type = script_manager_->find_script_type(script_entry.type_name);
            if (!script_type) continue;

            auto* base = reinterpret_cast<uint8_t*>(script_entry.instance.get());

            for (const auto& field : script_type->fields) {
                if (field.type != FJELL_FIELD_WIDGET_PTR) continue;

                for (const auto& child : scene.objects()) {
                    if (child->parent_id != static_cast<int32_t>(obj->id)) continue;
                    if (child->name != field.name) continue;

                    auto it = node_to_widget.find(child->id);
                    if (it != node_to_widget.end()) {
                        auto** ptr = reinterpret_cast<UiWidget**>(base + field.offset);
                        *ptr = it->second;
                        FJELL_CORE_INFO("UiSubsystem: bound {}.{} -> widget on '{}'",
                                        script_entry.type_name, field.name, child->name);
                    }
                    break;
                }
            }
        }
    }
}

void UiSubsystem::collect_widgets(UiNode& node, UiInstance& instance) {
    if (node.is_widget && node.scoped_context && !node.widget_tag.empty()) {
        auto* ui_type = script_manager_->find_ui_widget_type(node.widget_tag);
        if (ui_type && ui_type->create) {
            WidgetInstance wi;
            wi.type = ui_type;
            wi.widget_context = node.scoped_context.get();
            wi.widget.reset(ui_type->create());

            node.has_controller = true;

            FJELL_CORE_INFO("UiSubsystem: created widget '{}' for <{}>",
                            ui_type->name, node.widget_tag);

            instance.widgets.push_back(std::move(wi));
        }
    }

    for (auto& child : node.children) {
        collect_widgets(*child, instance);
    }
}

void UiSubsystem::sync_bind_fields(WidgetInstance& wi) {
    if (!wi.widget || !wi.type || !wi.widget_context) return;

    auto* base = reinterpret_cast<const uint8_t*>(wi.widget.get());

    for (const auto& field : wi.type->fields) {
        const void* ptr = base + field.offset;
        const char* key = field.name.c_str();

        switch (field.type) {
            case FJELL_FIELD_FLOAT:
                wi.widget_context->set_float(key, *static_cast<const float*>(ptr));
                break;
            case FJELL_FIELD_INT: {
                int val = *static_cast<const int*>(ptr);
                wi.widget_context->set_int(key, val);
                break;
            }
            case FJELL_FIELD_BOOL:
                wi.widget_context->set_bool(key, *static_cast<const bool*>(ptr));
                break;
            case FJELL_FIELD_STRING: {
                const auto* str = reinterpret_cast<const std::string*>(ptr);
                wi.widget_context->set(key, *str);
                break;
            }
            default:
                break;
        }
    }
}

void UiSubsystem::tick(const TickContext& ctx) {
    for (auto& inst : instances_) {
        for (auto& wi : inst.widgets) {
            if (!wi.widget) continue;
            wi.widget->tick(ctx.dt);
            sync_bind_fields(wi);
        }
    }

    // Rebuild world-space UI records for rendering
    world_records_.clear();
    for (auto& inst : instances_) {
        if (inst.render_mode != UiWidgetComponent::RenderMode::world_space) continue;
        if (!inst.game_ui || !inst.game_ui->loaded()) continue;

        auto* spatial = ctx.scene.find_spatial(inst.node_id);
        if (!spatial) continue;

        const auto* wc = ctx.scene.find(inst.node_id)->components.get<UiWidgetComponent>();
        if (!wc) continue;

        // World position with local offset
        glm::vec3 world_pos = glm::vec3(spatial->world_matrix_[3])
                            + glm::mat3(spatial->world_matrix_) * wc->offset;

        // Build world transform (billboard or node rotation)
        glm::mat4 world_transform;
        if (wc->billboard) {
            glm::vec3 to_camera = camera_position_ - world_pos;
            float dist = glm::length(to_camera);
            if (dist > 0.001f) {
                to_camera /= dist;
            } else {
                to_camera = glm::vec3(0.0f, 0.0f, 1.0f);
            }
            glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
            glm::vec3 right = glm::normalize(glm::cross(up, to_camera));
            up = glm::cross(to_camera, right);
            world_transform = glm::mat4(
                glm::vec4(right, 0.0f),
                glm::vec4(up, 0.0f),
                glm::vec4(to_camera, 0.0f),
                glm::vec4(world_pos, 1.0f)
            );
        } else {
            world_transform = spatial->world_matrix_;
            world_transform[3] = glm::vec4(world_pos, 1.0f);
        }

        UiWorldPushConstants push{};
        push.mvp = projection_matrix_ * view_matrix_ * world_transform;
        push.pixels_per_unit = wc->pixels_per_unit;
        push.viewport_width = wc->virtual_viewport.x;
        push.viewport_height = wc->virtual_viewport.y;

        world_records_.push_back({inst.game_ui.get(), push});
    }
}

void UiSubsystem::set_camera(const glm::vec3& pos, const glm::mat4& view, const glm::mat4& proj) {
    camera_position_ = pos;
    view_matrix_ = view;
    projection_matrix_ = proj;
}

void UiSubsystem::on_play_stop(const TickContext& /*ctx*/) {
    world_records_.clear();
    for (auto& inst : instances_) {
        for (auto& wi : inst.widgets) {
            if (wi.widget) {
                wi.widget->end_play();
            }
            wi.connections.clear();
            wi.widget.reset();
        }
    }

    vulkan_context_.wait_idle();

    for (auto& inst : instances_) {
        inst.game_ui->destroy();
    }
    instances_.clear();
}

} // namespace fjell
