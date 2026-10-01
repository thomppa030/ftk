#include "ui/properties_panel.hpp"
#include "ftk/app/command_history.hpp"
#include "core/commands.hpp"
#include "ftk/base/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/uniform_types.hpp"
#include "scene/component_registry.hpp"
#include "scene/scene.hpp"
#include "scripting/script_commands.hpp"
#include "scripting/script_manager.hpp"
#include "ui/inspectors/inspector_registry.hpp"
#include "ui/inspectors/script_fields.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/component_block.hpp"
#include "ftk/ui/kit/edit_record.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/inset_group.hpp"
#include "ui/icons.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ftk/ui/kit/grouped_picker.hpp"
#include "ui/material_params_widget.hpp"
#include "ui/shader_error_callout.hpp"

#include <imgui.h>

#include <algorithm>

namespace fjell {

void PropertiesPanel::init(Scene* scene, uint32_t* selected_id,
                           ScriptManager* scripts,
                           ftk::CommandHistory* history, GridSettings* grid_settings,
                           PipelineCache* pipeline_cache,
                           BindlessTextures* bindless,
                           ResourceManager* resource_manager,
                           ProjectManager* project_mgr,
                           ContentBrowser* content_browser,
                           ftk::IconCache* icons, TerrainSculptTool* sculpt) {
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
    if (auto p = ftk::ui::Panel(icon::properties, "Properties")) {
        Node* base = scene_->find(*selected_id_);
        if (base == nullptr) {
            ftk::ui::empty_state(icon::nothing_selected, "Nothing selected",
                            "Pick an object in the viewport or the hierarchy to edit it here");
            return;
        }
        draw_name(*base);
        Node3D* node = base->as_spatial();
        if (node == nullptr) return;

        const InspectorContext ictx{
            scene_, history_, resource_manager_, pipeline_cache_, bindless_, project_mgr_,
            content_browser_, icons_, sculpt_};
        draw_transform(*node);
        // The node's own settings come before what is composed onto it.
        draw_node_sections(*node, ictx);
        draw_components(*node, ictx);
        draw_scripts(*node, ictx);
        draw_add(*node, ictx);
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
    if (auto t = ftk::ui::PropertyTable("##name")) {
        ftk::ui::row("Name", [&] {
            if (name_field_.draw("##Name", named->name).committed) {
                commit_name();
            }
        });
    }
}

void PropertiesPanel::draw_components(Node3D& node, const InspectorContext& ictx) {
    ftk::ui::section("Components", icon::components);
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
        const auto category = inspector ? inspector->category : ftk::theme::Category::Structure;
        if (ftk::ui::component_block(meta->display_name.c_str(), category, remove)) {
            // The fields in one group, so the block knows whether
            // one of them is being edited or under the mouse.
            const std::string scope = std::to_string(node.id) + "/" + meta->json_key;
            ftk::ui::edit_scope(scope);
            ImGui::BeginGroup();
            const ftk::ui::Edit edit = inspector ? inspector->draw(node, ptr, ictx) : ftk::ui::Edit{};
            ImGui::EndGroup();
            ftk::ui::edit_scope({});
            const bool active = ImGui::IsItemActive();
            const bool hovered = ImGui::IsItemHovered();
            component_edits_.track(
                *scene_, history_, node.id, id, {edit.changed, edit.committed, active, hovered},
                [&] { return ComponentState::of(node, id); },
                [&](ComponentState before, ComponentState after) {
                    return std::make_unique<ComponentEditCommand>(
                        *scene_, node.id, id, std::move(before), std::move(after),
                        edit_description(node.name + " \xc2\xb7 " + meta->display_name, scope));
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
}

void PropertiesPanel::draw_scripts(Node3D& node, const InspectorContext& ictx) {
    ftk::ui::section("Scripts", icon::scripts);
    std::string remove;
    for (auto& entry : node.scripts) {
        ImGui::PushID(entry.type_name.c_str());
        bool removed = false;
        if (ftk::ui::component_block(entry.type_name.c_str(), ftk::theme::Category::Logic, removed)) {
            draw_script(node, entry, ictx);
        }
        if (removed) remove = entry.type_name;
        ImGui::PopID();
    }
    if (!remove.empty()) {
        if (history_) {
            history_->execute(std::make_unique<RemoveScriptCommand>(*scene_, *scripts_, node.id, remove));
        } else {
            scripts_->remove_script(node, remove);
            scene_->mark_structure_changed();
            scene_->mark_dirty();
        }
    }
}

namespace {

constexpr const char* ADD_PICKER = "##add";
constexpr std::size_t RECENT_COUNT = 4;

} // namespace

std::string PropertiesPanel::edit_description(const std::string& subject, const std::string& scope) {
    if (auto record = ftk::ui::take_edit_record(scope)) {
        return ftk::describe_change(subject + ": " + record->label, record->before, record->after);
    }
    return "Edit " + subject;
}

void PropertiesPanel::draw_add(Node3D& node, const InspectorContext& ictx) {
    ImGui::Dummy({0.0f, ftk::theme::GAP_M});
    if (ftk::ui::action(icon::add, "Add component", ftk::ui::ButtonKind::Secondary, {-1.0f, 0.0f})) {
        ftk::ui::grouped_picker::open(ADD_PICKER);
    }
    // Gathered only while the picker is up; drawn every frame so it hangs
    // under the button.
    std::vector<ftk::ui::grouped_picker::Group> groups;
    if (ftk::ui::grouped_picker::is_open(ADD_PICKER)) groups = addable_groups(node);
    std::string picked;
    if (ftk::ui::grouped_picker::draw(ADD_PICKER, groups,
                             {.search_hint = "Search components and scripts",
                              .empty_message = "Nothing matches"},
                             picked)) {
        add_to_node(node, picked, ictx);
    }
}

std::vector<ftk::ui::grouped_picker::Group> PropertiesPanel::addable_groups(const Node3D& node) const {
    using ftk::ui::grouped_picker::Group;
    using ftk::ui::grouped_picker::Item;
    const std::string owner = node.name.empty() ? std::string("This node") : node.name;

    // Everything that can be added, by value, with the category it goes under.
    std::vector<std::pair<Item, ftk::theme::Category>> all;
    const auto& inspectors = component_inspectors();
    ComponentRegistry::instance().for_each([&](ComponentTypeId id, const ComponentMeta& meta) {
        const auto* inspector = inspectors.find(id);
        Item item{.value = "component:" + meta.json_key, .label = meta.display_name};
        if (node.components.get_by_id(id) != nullptr) {
            item.enabled = false;
            item.detail = "added";
            item.disabled_reason = owner + " already has a " + meta.display_name;
        }
        all.emplace_back(std::move(item), inspector ? inspector->category : ftk::theme::Category::Structure);
    });
    if (scripts_ != nullptr && scripts_->is_loaded()) {
        for (const auto& type : scripts_->script_types()) {
            if (!type.loaded) continue;
            Item item{.value = "script:" + type.name, .label = type.name + " (script)"};
            const bool present = std::ranges::any_of(
                node.scripts, [&](const ScriptEntry& e) { return e.type_name == type.name; });
            if (present) {
                item.enabled = false;
                item.detail = "added";
                item.disabled_reason = owner + " already has " + type.name;
            }
            all.emplace_back(std::move(item), ftk::theme::Category::Logic);
        }
    }
    std::ranges::sort(all, [](const auto& a, const auto& b) { return a.first.label < b.first.label; });

    std::vector<Group> groups;
    // What was added last comes first, each with its category's dot.
    Group recent{.label = "Recently added"};
    for (const auto& value : recent_) {
        auto it = std::ranges::find_if(all, [&](const auto& e) { return e.first.value == value; });
        if (it == all.end()) continue;
        Item item = it->first;
        item.category = it->second;
        recent.items.push_back(std::move(item));
    }
    if (!recent.items.empty()) groups.push_back(std::move(recent));

    for (int c = 0; c <= static_cast<int>(ftk::theme::Category::Structure); ++c) {
        const auto category = static_cast<ftk::theme::Category>(c);
        Group group{.label = ftk::theme::category_name(category), .category = category};
        for (const auto& [item, item_category] : all) {
            if (item_category == category) group.items.push_back(item);
        }
        if (!group.items.empty()) groups.push_back(std::move(group));
    }
    return groups;
}

void PropertiesPanel::add_to_node(Node3D& node, const std::string& value, const InspectorContext& ictx) {
    constexpr std::string_view COMPONENT = "component:";
    constexpr std::string_view SCRIPT = "script:";
    if (value.starts_with(COMPONENT)) {
        const std::string key = value.substr(COMPONENT.size());
        auto& registry = ComponentRegistry::instance();
        const ComponentMeta* found = nullptr;
        ComponentTypeId found_id = 0;
        registry.for_each([&](ComponentTypeId id, const ComponentMeta& meta) {
            if (meta.json_key == key) {
                found = &meta;
                found_id = id;
            }
        });
        if (found == nullptr) return;
        auto add = [init = found->init, ctx = ictx](Node3D& n) { init(n, ctx); };
        if (history_) {
            history_->execute(std::make_unique<AddComponentCommand>(*scene_, node.id, found_id, add,
                                                                    found->display_name));
        } else {
            add(node);
            scene_->mark_dirty();
        }
    } else if (value.starts_with(SCRIPT)) {
        const std::string type_name = value.substr(SCRIPT.size());
        if (history_) {
            history_->execute(std::make_unique<AddScriptCommand>(*scene_, *scripts_, node.id, type_name));
        } else {
            (void)scripts_->add_script(node, type_name);
            scene_->mark_structure_changed();
            scene_->mark_dirty();
        }
    } else {
        return;
    }
    std::erase(recent_, value);
    recent_.insert(recent_.begin(), value);
    if (recent_.size() > RECENT_COUNT) recent_.resize(RECENT_COUNT);
}

void PropertiesPanel::draw_script(Node3D& node, ScriptEntry& entry, const InspectorContext& ictx) {
    // A script without an instance keeps its saved fields; the block says
    // why they can't be edited instead of showing nothing.
    const auto* type = scripts_->find_script_type(entry.type_name);
    if (!scripts_->is_loaded()) {
        ftk::ui::status(ftk::ui::Severity::Warning, "Scripts aren't built yet: Scripts > Build & Reload");
        return;
    }
    if (type == nullptr || !type->loaded) {
        ftk::ui::status(ftk::ui::Severity::Warning, "Not in the last build. Its values are kept until it is back");
        return;
    }
    if (!entry.instance) {
        ftk::ui::status(ftk::ui::Severity::Error, "Couldn't be created: Scripts > Build & Reload tries again");
        return;
    }
    if (type->fields.empty()) {
        ftk::ui::hint("No properties");
        return;
    }

    // The fields in one group, so the block knows whether one of them is
    // being edited or under the mouse.
    const std::string scope = std::to_string(node.id) + "/script/" + entry.type_name;
    ftk::ui::edit_scope(scope);
    ImGui::BeginGroup();
    const ftk::ui::Edit edit = draw_script_fields(type->fields, entry.instance.get(), ictx, scene_);
    ImGui::EndGroup();
    ftk::ui::edit_scope({});
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    script_edits_.track(
        *scene_, history_, node.id, ScriptManager::hash_type_name(entry.type_name),
        {edit.changed, edit.committed, active, hovered},
        [&] { return scripts_->fields_of(entry); },
        [&](nlohmann::json before, nlohmann::json after) {
            return std::make_unique<ScriptEditCommand>(
                *scene_, *scripts_, node.id, entry.type_name, std::move(before), std::move(after),
                edit_description(node.name + " \xc2\xb7 " + entry.type_name, scope));
        });
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

    // The material's values are its own, shared by every node using it, so
    // they are set apart from the component's in a box of their own.
    auto box = ftk::ui::InsetGroup("##material_values", "Material", icon::material,
                              ftk::theme::category(ftk::theme::Category::Rendering));
    if (!box) return;
    // Its values stay editable: they are the last version that compiled.
    shader_error_callout(*pipeline_cache_, pid, project_mgr_ != nullptr ? project_mgr_->active() : nullptr);
    if (auto t = ftk::ui::PropertyTable("##material")) {
        ftk::ui::row("Shader", [&] { ftk::ui::readout(mat->shader_path().c_str()); });
        // UV scale: every surface material has one; a ui quad's UV is its own
        // and a terrain material tiles each layer by that layer's own scale.
        if (meta.shader_type == ShaderType::Spatial) {
            const auto& gpu = bindless_->material_at(mat->material_id());
            glm::vec2 uv_scale{gpu.base_params[7], gpu.base_params[8]};
            if (uv_scale.x == 0.0f) uv_scale.x = 1.0f;
            if (uv_scale.y == 0.0f) uv_scale.y = 1.0f;
            ftk::ui::row("UV scale", [&] {
                if (ftk::ui::vec2("##uv_scale", uv_scale, {.speed = 0.1f, .min = 0.01f, .max = 100.0f,
                                                      .unit = ftk::ui::Unit::Times})
                        .changed) {
                    GpuMaterial updated = gpu;
                    updated.base_params[7] = uv_scale.x;
                    updated.base_params[8] = uv_scale.y;
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
