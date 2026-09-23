#include "ui/properties_panel.hpp"
#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "core/log.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/component_registry.hpp"
#include "scene/scene.hpp"
#include "scripting/script_manager.hpp"
#include "ui/icons_lc.hpp"
#include "ui/inspector_widgets.hpp"
#include "ui/inspectors/inspector_registry.hpp"
#include "ui/kit/component_block.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/kit/text_field.hpp"
#include "ui/material_params_widget.hpp"
#include "ui/panel_widget.hpp"

#include <imgui.h>

#include <algorithm>

namespace fjell {

void PropertiesPanel::init(Scene* scene, uint32_t* selected_id,
                           ScriptManager* scripts,
                           CommandHistory* history, GridSettings* grid_settings,
                           PipelineCache* pipeline_cache,
                           BindlessTextures* bindless,
                           ResourceManager* resource_manager,
                           ProjectManager* project_mgr,
                           ContentBrowser* content_browser,
                           FileBrowser* file_browser,
                           IconCache* icons, TerrainSculptTool* sculpt) {
    scene_ = scene;
    selected_id_ = selected_id;
    scripts_ = scripts;
    history_ = history;
    grid_settings_ = grid_settings;
    pipeline_cache_ = pipeline_cache;
    bindless_ = bindless;
    resource_manager_ = resource_manager;
    project_mgr_ = project_mgr;
    content_browser_ = content_browser;
    file_browser_ = file_browser;
    icons_ = icons;
    sculpt_ = sculpt;
}

void PropertiesPanel::commit_name() {
    auto* node = scene_->find(name_node_id_);
    const std::string& new_name = name_field_.text();
    if (node == nullptr || new_name.empty() || new_name == node->name) return;
    if (history_) {
        history_->execute(std::make_unique<RenameCommand>(
            *scene_, node->id, node->name, new_name));
    } else {
        node->name = new_name;
        scene_->mark_dirty();
    }
}

void PropertiesPanel::draw() {
    if (auto p = Panel(ui::icon::properties, "Properties")) {
        Node* base = scene_->find(*selected_id_);
        if (base == nullptr) {
            ui::empty_state(ui::icon::nothing_selected, "Nothing selected",
                            "Pick an object in the viewport or the hierarchy to edit it here");
            return;
        }
        draw_name(*base);
        Node3D* node = base->as_spatial();
        if (node == nullptr) return;

        const InspectorContext ictx{
            scene_, history_, resource_manager_, pipeline_cache_, bindless_, project_mgr_,
            content_browser_, file_browser_, icons_, sculpt_};
        draw_transform(*node);
        // The node's own settings come before what is composed onto it.
        draw_node_sections(*node, ictx);
        draw_components(*node, ictx);
        draw_scripts(*node);
    }
}

void PropertiesPanel::draw_name(const Node& selected) {
    // Editable name: one undoable rename, on Enter or when the field loses
    // focus. The node is taken when the edit starts, since the click that
    // ends it can select another node first.
    if (!name_field_.editing()) {
        name_node_id_ = selected.id;
    }
    const Node* named = scene_->find(name_node_id_);
    if (named == nullptr) {
        named = &selected;
        name_node_id_ = selected.id;
    }
    if (auto t = ui::PropertyTable("##name")) {
        ui::row("Name", [&] {
            if (name_field_.draw("##Name", named->name).committed) {
                commit_name();
            }
        });
    }
}

void PropertiesPanel::draw_components(Node3D& node, const InspectorContext& ictx) {
    ui::section("Components", ui::icon::components);
    auto& registry = ComponentRegistry::instance();
    ComponentTypeId remove_id = ~0u;

    const auto& inspectors = component_inspectors();
    node.components.for_each([&](ComponentTypeId id, void* ptr) {
        const auto* meta = registry.find(id);
        if (!meta) return;
        const auto* inspector = inspectors.find(id);

        // Each component's widgets get their own ID scope: two
        // components on one node may both have an "Add" or "Clear".
        ImGui::PushID(meta->json_key.c_str());
        bool remove = false;
        const auto category = inspector ? inspector->category : theme::Category::Structure;
        if (ui::component_block(meta->display_name.c_str(), category, remove)) {
            // The fields in one group, so the block knows whether
            // one of them is being edited or under the mouse.
            ImGui::BeginGroup();
            const bool kit = inspector && inspector->draw;
            ui::Edit edit;
            if (kit) {
                edit = inspector->draw(node, ptr, ictx);
            } else {
                edit.changed = meta->draw(node, ptr, ictx);
            }
            ImGui::EndGroup();
            const bool active = ImGui::IsItemActive();
            const bool hovered = ImGui::IsItemHovered();

            // A kit inspector says when its edit is finished. One
            // drawn by its ComponentMeta only says it changed, so
            // the edit is taken as finished once nothing in the
            // block is held and no picker or list is open over it.
            const bool committed = kit
                ? edit.committed
                : !active && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
            component_edits_.track(
                *scene_, history_, node.id, id, {edit.changed, committed, active, hovered},
                [&] {
                    ComponentStorage copy;
                    node.components.clone_one(id, node.components, copy);
                    return copy;
                },
                [&](ComponentStorage before, ComponentStorage after) {
                    return std::make_unique<ComponentEditCommand>(
                        *scene_, node.id, id, std::move(before), std::move(after),
                        "Edit " + meta->display_name);
                });

            // A component that paints itself with a material shows
            // that material's own parameters here, so a surface is
            // tuned where it is placed.
            if (meta->material != nullptr) {
                draw_material_inspector_for(meta->material(ptr));
            }
        }
        if (remove) {
            remove_id = id;
        }
        ImGui::PopID();
    });

    if (remove_id != ~0u) {
        if (history_) {
            const auto* meta = registry.find(remove_id);
            history_->execute(std::make_unique<RemoveComponentCommand>(
                *scene_, node.id, remove_id, meta ? meta->display_name : "component"));
        } else {
            node.components.remove_by_id(remove_id);
            scene_->mark_dirty();
        }
    }


    // Add Component popup — lists types not already present
    if (ImGui::Button(ICON_LC_PLUS "  Add Component", {-1, 0})) {
        ImGui::OpenPopup("##AddBuiltinComponent");
    }
    if (ImGui::BeginPopup("##AddBuiltinComponent")) {
        registry.for_each([&](ComponentTypeId id, const ComponentMeta& meta) {
            if (!node.components.get_by_id(id) && ImGui::MenuItem(meta.display_name.c_str())) {
                meta.init(node, ictx);
                scene_->mark_dirty();
            }
        });
        ImGui::EndPopup();
    }
}

void PropertiesPanel::draw_scripts(Node3D& node) {
    ui::section("Scripts", ui::icon::scripts);
    if (scripts_->draw_inspector(node)) {
        scene_->mark_dirty();
    }

    // Add script button
    if (scripts_->is_loaded() && !scripts_->script_types().empty()) {
        if (ImGui::Button(ICON_LC_PLUS "  Add Script", {-1, 0})) {
            ImGui::OpenPopup("##AddScript");
        }
        if (ImGui::BeginPopup("##AddScript")) {
            for (const auto& type : scripts_->script_types()) {
                if (type.loaded && ImGui::MenuItem(type.name.c_str())) {
                    if (auto r = scripts_->add_script(node, type.name); !r) {
                        FJELL_CORE_ERROR("{}", r.error());
                    }
                }
            }
            ImGui::EndPopup();
        }
    }
}

void PropertiesPanel::draw_material_inspector_for(Material* mat) {
    if (!mat || !pipeline_cache_ || !bindless_) return;

    uint32_t pid = mat->pipeline_id();
    if (pid >= pipeline_cache_->shader_count()) return;

    const auto& meta = pipeline_cache_->metadata(pid);

    // Only show if shader has custom uniforms
    bool has_custom = false;
    for (const auto& u : meta.uniforms) {
        if (u.type != UniformType::Sampler2D) {
            has_custom = true;
            break;
        }
    }

    ImGui::TextDisabled(ICON_LC_FILE_CODE "  Shader: %s", mat->shader_path().c_str());

    // UV scale: every surface material has one; a ui quad's UV is its own and
    // a terrain material tiles each layer by that layer's own scale
    if (meta.shader_type == ShaderType::Spatial) {
        const auto& gpu = bindless_->material_at(mat->material_id());
        float uv_sc[2] = {gpu.base_params[7], gpu.base_params[8]};
        if (uv_sc[0] == 0.0f) uv_sc[0] = 1.0f;
        if (uv_sc[1] == 0.0f) uv_sc[1] = 1.0f;
        if (auto t = PropertyTable("##mat_uv")) {
            prop_row("UV Scale", [&] {
                if (ImGui::DragFloat2("##UVScale", uv_sc, 0.1f, 0.01f, 100.0f)) {
                    GpuMaterial updated = gpu;
                    updated.base_params[7] = uv_sc[0];
                    updated.base_params[8] = uv_sc[1];
                    bindless_->update_material(mat->material_id(), updated);
                    scene_->mark_dirty();
                }
            });
        }
    }

    if (!has_custom) return;

    if (draw_material_params(meta, *bindless_, *mat)) {
        scene_->mark_dirty();
    }
}

} // namespace fjell
