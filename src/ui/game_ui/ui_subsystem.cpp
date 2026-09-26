#include "ui/game_ui/ui_subsystem.hpp"
#include "core/base_path.hpp"
#include "core/frame_context.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/project.hpp"
#include "net/net_manager.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/resources/texture.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "scripting/fjell/api.h"
#include "ui/game_ui/game_model_world.hpp"
#include "ui/game_ui/ui_actions.hpp"
#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/ui_widget_component.hpp"
#include "ui/game_ui/widget_asset.hpp"
#include "scripting/script_manager.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <filesystem>

namespace fjell {

UiSubsystem::UiSubsystem(VulkanContext& vulkan_context, ScriptManager& script_manager)
    : vulkan_context_{vulkan_context}, script_manager_{&script_manager} {}

/// Loads the .fjwidget an instance was placed with, rejecting one that
/// parsed but carries no markup — there would be nothing to show for it.
static Result<WidgetAsset> load_instance_widget(const std::string& path) {
    auto result = load_widget_asset(path);
    if (result && !result->markup_root) {
        return make_error("widget has no markup: " + path);
    }
    return result;
}

/// The design frame a placement is scaled by (see UiDesignFrame). Only
/// screen-space placements scale: a world-space widget lays out in its own
/// virtual viewport and is sized by the quad it is drawn on.
static UiDesignFrame design_frame_for(const UiWidgetComponent& wc, const Project* project) {
    if (wc.render_mode == UiWidgetComponent::RenderMode::world_space) return {};
    switch (wc.scaling) {
    case UiWidgetComponent::Scaling::design_resolution:
        if (!project) return {};
        return {static_cast<float>(project->config().ui.design_width),
                static_cast<float>(project->config().ui.design_height)};
    case UiWidgetComponent::Scaling::native:
        return {};
    case UiWidgetComponent::Scaling::custom:
        return {0.0f, wc.custom_reference_height};
    }
    return {};
}

void UiSubsystem::on_play_start(const TickContext& ctx) {
    instances_.clear();
    world_records_.clear();
    scene_ = &ctx.scene;
    model_world_ = std::make_unique<GameModelWorld>(*script_manager_, net_manager_);

    for (const auto& obj : ctx.scene.objects()) {
        const auto* wc = obj->components.get<UiWidgetComponent>();
        if (!wc || wc->widget_path.empty()) continue;

        // widget_path may be a project-relative path or an asset id (see
        // UiWidgetComponent's serialize, which writes whichever the project's
        // asset database gives it) — resolve_asset handles both, the same way
        // mesh/skeletal components resolve their own asset references at the
        // point of use.
        std::string widget_resolved = base_path::resolve_asset(wc->widget_path, ctx.project);

        auto widget_result = load_instance_widget(widget_resolved);
        if (!widget_result) {
            FJELL_CORE_WARN("UiSubsystem: {}", widget_result.error());
            continue;
        }

        UiInstance inst;
        inst.node_id = obj->id;
        inst.widget_path = widget_resolved;
        inst.style_classes = wc->style_classes;
        inst.model_nodes = wc->model_nodes;
        inst.render_mode = wc->render_mode;
        inst.game_ui = std::make_unique<GameUi>();
        inst.game_ui->pipes().set_game_pipes(&script_manager_->reflected_types());
        // Both draw into an OffscreenPass, never the swapchain — world
        // widgets into the scene's HDR target, screen widgets into
        // PlayController's HUD target — so the pipeline's color format has
        // to match that.
        vulkan_context_.init_game_ui(*inst.game_ui, OffscreenPass::COLOR_FORMAT,
            inst.render_mode == UiWidgetComponent::RenderMode::world_space);
        inst.game_ui->set_project(ctx.project);
        UiDesignFrame design_frame = design_frame_for(*wc, ctx.project);
        inst.game_ui->set_design_frame(design_frame);
        if (resource_manager_) inst.game_ui->set_resource_manager(resource_manager_);
        GameUi* ui = inst.game_ui.get();
        ui->set_repeat_hooks([this, ui](UiNode& repeat) { repeat_stamped(*ui, repeat); },
                             [this, ui](UiNode& repeat) { repeat_removing(*ui, repeat); });

        // Registered before its tree loads: loading binds, and binding can
        // stamp repeats whose controllers the hooks create.
        instances_.push_back(std::move(inst));
        UiInstance& placed = instances_.back();
        apply_widget(placed, *widget_result);

        std::string classes;
        for (const auto& cls : placed.style_classes) classes += " ." + cls;
        FJELL_CORE_INFO("UiSubsystem: loaded widget '{}' for node {} ({} sub-widgets{}{}, {})",
                        wc->widget_path, obj->id, placed.widgets.size(),
                        classes.empty() ? "" : ", classes:", classes,
                        design_frame.height > 0.0f
                            ? "scaled to fit " + std::to_string(static_cast<int>(design_frame.width)) + "x" +
                                  std::to_string(static_cast<int>(design_frame.height))
                            : std::string("native pixels"));
    }

    // Resolve [[fj::bind]] UiWidget* pointers on ScriptComponents
    resolve_widget_bindings(ctx.scene);
}

void UiSubsystem::apply_widget(UiInstance& inst, const WidgetAsset& widget_asset) {
    // Resolve .fjss path relative to the widget file's directory
    auto widget_dir = std::filesystem::path(inst.widget_path).parent_path();
    std::string fjss_path;
    if (!widget_asset.fjss_path.empty()) {
        fjss_path = (widget_dir / widget_asset.fjss_path).string();
    }
    if (!fjss_path.empty() && !std::filesystem::exists(fjss_path)) {
        fjss_path.clear();
    }

    // Controllers answer actions in contexts owned by the tree that is about
    // to be replaced, so they go first, while those contexts still exist.
    // Models are bound into those contexts, and go with them.
    destroy_controllers(inst);
    inst.game_ui->models().clear();

    // Deep-clone the markup tree (each instance needs its own mutable copy)
    auto tree_clone = UiDataBind::clone_node(*widget_asset.markup_root, nullptr);
    inst.game_ui->load_from_tree(std::move(tree_clone), fjss_path, widget_dir.string(),
                                 inst.widget_path);

    // Per-instance classes land on the root node, so one placement can pick
    // a variant the widget's own sheet defines (".compact", ".enemy")
    // without needing a separate .fjwidget per look.
    if (auto* root = inst.game_ui->root()) {
        for (const auto& cls : inst.style_classes) {
            if (!root->has_class(cls)) root->classes.push_back(cls);
        }
    }

    // Apply <prop> defaults from the .fjwidget to the root context. Child
    // widgets get this via widget_resolver; the root widget's context is
    // game_ui's top-level one, so we do it here.
    set_prop_defaults(widget_asset.props, inst.game_ui->context());
    collect_models(inst, widget_asset);

    inst.game_ui->mark_dirty();

    create_controllers(inst, widget_asset);
}

void UiSubsystem::collect_models(UiInstance& inst, const WidgetAsset& widget_asset) {
    auto& models = inst.game_ui->models();
    if (scene_ != nullptr && model_world_) models.attach(*scene_, *model_world_, inst.node_id);

    // The placed widget's own Models live in the UI's root scope, each read
    // from the node the placement picked for it when there is one; a nested
    // widget's in the scope WidgetResolver made for it.
    for (const auto& model : widget_asset.models) {
        uint32_t placed = 0;
        for (const auto& picked : inst.model_nodes) {
            if (picked.model == model.name) placed = picked.node;
        }
        models.add(model, inst.game_ui->context(), placed);
    }
    if (auto* root = inst.game_ui->root()) models.add_widgets(*root);
}

void UiSubsystem::read_controller_in_place(WidgetInstance& wi) {
    // A controller is created after the tree and destroyed before it, so its
    // scope never reads one that is gone. A prop of the same name as one of
    // its fields is what markup reads, so the field would never show.
    UiContext& scope = *wi.widget_context;
    for (const auto& field : wi.type->fields) {
        if (scope.find(field.name) != nullptr) {
            FJELL_CORE_WARN("Widget '{}': the prop '{}' hides the controller's field of that name; "
                            "markup reads the prop",
                            wi.type->name, field.name);
        }
    }
    scope.set_self(UiObject{wi.type->name, wi.type->fields, wi.widget.get()});
}

void UiSubsystem::bind_models(const TickContext& /*ctx*/) {
    FJELL_PROFILE_SCOPE_N("ui_bind_models");
    for (auto& inst : instances_) {
        if (inst.game_ui) inst.game_ui->models().bind();
    }
}

void UiSubsystem::create_controllers(UiInstance& inst, const WidgetAsset& widget_asset) {
    if (!script_manager_ || !script_manager_->is_loaded()) return;

    // Create the top-level widget from .fjwidget class name
    if (!widget_asset.class_name.empty()) {
        auto* wt = script_manager_->find_ui_widget_type(widget_asset.class_name);
        if (!wt) {
            FJELL_CORE_WARN("UiSubsystem: widget class '{}' not found in script types",
                            widget_asset.class_name);
        }
        if (wt && wt->create) {
            WidgetInstance wi;
            wi.type = wt;
            wi.widget_context = &inst.game_ui->context();
            wi.widget.reset(wt->create());
            wi.widget->ui_context_.ctx_ = wi.widget_context;
            inst.widgets.push_back(std::move(wi));
            FJELL_CORE_INFO("UiSubsystem: created widget '{}'", widget_asset.class_name);
        }
    }

    // Also walk the resolved UI tree for sub-widgets (embedded via tags)
    auto* root = inst.game_ui->root();
    if (root) {
        collect_widgets(*root, inst);
    }

    check_actions(inst);
    for (auto& wi : inst.widgets) wire_controller(inst, wi);
    for (auto& wi : inst.widgets) wi.widget->begin_play();
}

void UiSubsystem::wire_controller(UiInstance& inst, WidgetInstance& wi) {
    wi.widget->models_.models_ = &inst.game_ui->models();
    wi.widget->models_.scope_ = wi.widget_context;
    read_controller_in_place(wi);

    // Its scope answers the actions its type declares and lets the rest
    // bubble up to the widgets around it. The entry lives in a list, so the
    // pointer holds as long as the controller does.
    WidgetInstance* target = &wi;
    wi.widget_context->set_action_handler([this, target](std::string_view action, std::span<const UiValue> args) {
        for (const auto& declared : target->type->actions) {
            if (declared.name != action) continue;
            if (const std::string why = call_action(declared, target->widget.get(), args); !why.empty()) {
                report_once("Widget '" + target->type->name + "': on:click " + why);
            }
            return true;
        }
        return false;
    });
}

void UiSubsystem::end_controller(WidgetInstance& wi) {
    if (!wi.widget) return;
    wi.widget->end_play();
    // A controller can bind input and set timers by its own pointer, like a
    // script; recreated on a hot reload mid-play, it has to take those with it.
    if (script_manager_ != nullptr) script_manager_->release(wi.widget.get());
    // Its scope may outlive it for a moment: nothing there may reach it.
    wi.widget_context->set_action_handler(nullptr);
    wi.widget_context->set_self({});
    wi.widget.reset();
}

void UiSubsystem::report_once(const std::string& message) {
    if (reported_.insert(message).second) FJELL_CORE_WARN("{}", message);
}

void UiSubsystem::check_actions(UiInstance& inst) {
    UiNode* root = inst.game_ui->root();
    if (root == nullptr) return;
    // Which controller answers in which scope.
    std::unordered_map<const UiContext*, const OwnedUiWidgetType*> answers;
    for (const auto& wi : inst.widgets) answers[wi.widget_context] = wi.type;

    // A call is answered by the nearest scope, from where it is raised
    // outward, whose controller declares the action: the way a click goes.
    auto check = [&](const UiNode& node, const std::string& text, const UiContext* from) {
        const UiActionCall call = parse_action_call(text);
        for (const UiContext* scope = from; scope != nullptr; scope = scope->parent()) {
            auto it = answers.find(scope);
            if (it == answers.end()) continue;
            for (const auto& declared : it->second->actions) {
                if (declared.name != call.name) continue;
                if (const std::string why = check_action_call(declared, call); !why.empty()) {
                    FJELL_CORE_WARN("{}:{}: \"{}\": {}", node.source_file, node.source_line, text, why);
                }
                return;
            }
        }
        FJELL_CORE_WARN("{}:{}: \"{}\": no controller around it has an [[fj::action]] named {}",
                        node.source_file, node.source_line, text, call.name);
    };
    // `scope` is what the node's calls are raised in; `widget` the nearest
    // widget around it, whose events a click inside may raise instead.
    auto walk = [&](auto& self, const UiNode& node, const UiContext* scope, const UiNode* widget) -> void {
        if (node.foreach_item_index >= 0) return; // a repeat's calls are its template's
        const UiContext* here = node.scoped_context ? node.scoped_context.get()
                              : node.repeat_scope  ? node.repeat_scope.get()
                                                   : scope;
        if (auto it = node.attributes.find("on:click"); it != node.attributes.end()) {
            const bool event = widget != nullptr && widget->event_map.contains(it->second);
            if (!event) check(node, it->second, here);
        }
        for (const auto& [event, raised] : node.event_map) check(node, raised, here);
        const UiNode* around = node.is_widget && !node.event_map.empty() ? &node : widget;
        for (const auto& child : node.children) self(self, *child, here, around);
    };
    walk(walk, *root, &inst.game_ui->context(), nullptr);
}

UiSubsystem::UiInstance* UiSubsystem::instance_of(const GameUi& ui) {
    for (auto& inst : instances_) {
        if (inst.game_ui.get() == &ui) return &inst;
    }
    return nullptr;
}

void UiSubsystem::repeat_stamped(GameUi& ui, UiNode& repeat) {
    UiInstance* inst = instance_of(ui);
    if (inst == nullptr) return;
    auto& models = inst->game_ui->models();
    models.add_widgets(repeat);
    std::vector<WidgetInstance*> created;
    auto walk = [&](auto& self, UiNode& node, bool root) -> void {
        if (!root && (node.is_foreach_template || node.foreach_item_index >= 0)) return; // announced on their own
        if (node.is_widget && node.scoped_context && script_manager_ && script_manager_->is_loaded()) {
            if (WidgetInstance* wi = add_controller(*inst, node)) created.push_back(wi);
        }
        for (auto& child : node.children) self(self, *child, false);
    };
    walk(walk, repeat, true);
    models.bind(); // after the frame's last destroy, as UI reading happens now
    for (WidgetInstance* wi : created) wire_controller(*inst, *wi);
    for (WidgetInstance* wi : created) wi->widget->begin_play();
}

void UiSubsystem::repeat_removing(GameUi& ui, UiNode& repeat) {
    UiInstance* inst = instance_of(ui);
    if (inst == nullptr) return;
    std::vector<const UiContext*> scopes;
    auto walk = [&](auto& self, const UiNode& node) -> void {
        if (node.scoped_context) scopes.push_back(node.scoped_context.get());
        for (const auto& child : node.children) self(self, *child);
    };
    walk(walk, repeat);
    inst->game_ui->models().remove_widgets(repeat);
    std::erase_if(inst->widgets, [&](WidgetInstance& wi) {
        if (std::ranges::find(scopes, wi.widget_context) == scopes.end()) return false;
        end_controller(wi);
        return true;
    });
}

void UiSubsystem::destroy_controllers(UiInstance& inst) {
    for (auto& wi : inst.widgets) end_controller(wi);
    inst.widgets.clear();
}

void UiSubsystem::reload_instance(UiInstance& inst) {
    auto widget_result = load_instance_widget(inst.widget_path);
    if (!widget_result) {
        // A half-typed edit is the usual reason a reload fails; the last
        // good tree stays up and the next save gets another chance.
        FJELL_CORE_WARN("UiSubsystem: reload skipped, {}", widget_result.error());
        return;
    }
    apply_widget(inst, *widget_result);
    FJELL_CORE_INFO("UiSubsystem: reloaded widget '{}' ({} sub-widgets)",
                    inst.widget_path, inst.widgets.size());
}

void UiSubsystem::resolve_widget_bindings(Scene& scene) {
    if (!script_manager_ || !script_manager_->is_loaded()) return;

    // Build lookup: node_id → UiWidget pointer
    std::unordered_map<uint32_t, UiWidget*> node_to_widget;
    for (auto& inst : instances_) {
        if (!inst.widgets.empty()) {
            node_to_widget[inst.node_id] = inst.widgets.front().widget.get();
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
    if (node.is_foreach_template || node.foreach_item_index >= 0) return; // repeat_stamped()'s
    if (node.is_widget && node.scoped_context) (void)add_controller(instance, node);
    for (auto& child : node.children) collect_widgets(*child, instance);
}

UiSubsystem::WidgetInstance* UiSubsystem::add_controller(UiInstance& inst, UiNode& node) {
    // The class the .fjwidget names, or else its tag's name.
    std::string type_name = node.widget_class;
    if (type_name.empty()) {
        type_name = node.widget_tag;
        std::replace(type_name.begin(), type_name.end(), '-', '_');
    }
    const auto* ui_type = type_name.empty() ? nullptr : script_manager_->find_ui_widget_type(type_name);
    if (ui_type == nullptr || ui_type->create == nullptr) return nullptr; // a widget with no controller

    WidgetInstance wi;
    wi.type = ui_type;
    wi.widget_context = node.scoped_context.get();
    wi.widget.reset(ui_type->create());
    wi.widget->ui_context_.ctx_ = wi.widget_context;
    node.has_controller = true;
    FJELL_CORE_INFO("UiSubsystem: created widget '{}' for <{}>", ui_type->name, node.widget_tag);
    return &inst.widgets.emplace_back(std::move(wi));
}

void UiSubsystem::tick(const TickContext& ctx) {
    bool any_reloaded = false;
    for (auto& inst : instances_) {
        if (inst.game_ui && inst.game_ui->consume_reload_request()) {
            reload_instance(inst);
            any_reloaded = true;
        }
        for (auto& wi : inst.widgets) {
            if (wi.widget) wi.widget->tick(ctx.dt);
        }
    }

    // Scripts hold raw pointers to root widgets; a rebuilt instance has new ones.
    if (any_reloaded) {
        resolve_widget_bindings(ctx.scene);
    }
}

void UiSubsystem::update_world_records(const TickContext& ctx) {
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

        world_records_.push_back({inst.game_ui.get(), push, inst.prev_mvp});
        inst.prev_mvp = push.mvp;
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
        destroy_controllers(inst);
    }

    vulkan_context_.wait_idle();

    for (auto& inst : instances_) {
        inst.game_ui->destroy();
    }
    instances_.clear();
    model_world_.reset();
    scene_ = nullptr;
}

} // namespace fjell
