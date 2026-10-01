#include "ui/ui_preview_data.hpp"

#include "core/base_path.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/inset_group.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ftk/ui/theme.hpp"
#include "scene/component_meta.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "scripting/reflected_types.hpp"
#include "scripting/script_manager.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ui/game_ui/ui_widget_component.hpp"
#include "ui/inspectors/model_fields.hpp"
#include "ui/inspectors/script_fields.hpp"
#include "ui/node_slot.hpp"

#include <algorithm>
#include <filesystem>
#include <new>

namespace fjell {

namespace {

bool is_value_type(const std::string& type) {
    return type == "float" || type == "int" || type == "bool" || type == "string";
}

/// Whether the node carries the script component, built and running.
bool carries(const Node& node, const std::string& type) {
    return std::ranges::any_of(node.scripts, [&](const ScriptEntry& entry) {
        return entry.instance && entry.type_name == type;
    });
}

/// The node whose UI Widget component places the widget at `path`; 0 for none.
uint32_t placing_node(const Scene& scene, const std::string& path, const Project* project) {
    const auto wanted = std::filesystem::path(path).lexically_normal();
    for (const auto& node : scene.objects()) {
        const auto* widget = node->components.get<UiWidgetComponent>();
        if (widget == nullptr || widget->widget_path.empty()) continue;
        if (std::filesystem::path(base_path::resolve_asset(widget->widget_path, project)).lexically_normal() == wanted) {
            return node->id;
        }
    }
    return 0;
}

} // namespace

// ── An object built here ────────────────────────────────────────────────

UiPreviewData::Owned::Owned(const OwnedTypeDesc& type)
    : type_{&type},
      storage_{::operator new(std::max<std::size_t>(type.size, 1), std::align_val_t{std::max<uint32_t>(type.align, 1)})} {
    type_->construct(storage_);
}

UiPreviewData::Owned::~Owned() {
    type_->destruct(storage_);
    ::operator delete(storage_, std::align_val_t{std::max<uint32_t>(type_->align, 1)});
}

void UiPreviewData::Owned::reset() {
    type_->destruct(storage_);
    type_->construct(storage_);
}

// ── What the widget declares ────────────────────────────────────────────

UiPreviewData::UiPreviewData(const ScriptManager& scripts, const Scene* scene)
    : scripts_{scripts}, scene_{scene}, world_{scripts, nullptr} {}

UiPreviewData::~UiPreviewData() = default;

UiPreviewData::Entry UiPreviewData::owned_entry(const std::string& name, const OwnedTypeDesc& type,
                                                std::vector<Entry>& before) {
    Entry entry{.kind = Entry::Kind::owned, .name = name, .type = type.name};
    for (auto& old : before) {
        if (old.kind == Entry::Kind::owned && old.name == name && old.object && &old.object->type() == &type) {
            entry.object = std::move(old.object);
        }
    }
    // Built only from a type that can build one: a module too old to say how
    // leaves the object out.
    if (!entry.object && type.construct != nullptr && type.destruct != nullptr) {
        entry.object = std::make_unique<Owned>(type);
    }
    return entry;
}

void UiPreviewData::set_widget(const WidgetAsset& widget, const std::string& path, const Project* project) {
    std::vector<Entry> models = std::move(models_);
    std::vector<Entry> props = std::move(props_);
    models_.clear();
    props_.clear();
    widget_path_ = path;
    project_ = project;
    looked_ = false;
    auto picked_before = [](const std::vector<Entry>& before, const std::string& name, const std::string& type) {
        for (const auto& old : before) {
            if (old.kind == Entry::Kind::component && old.name == name && old.type == type) return old.picked;
        }
        return uint32_t{0};
    };

    for (const auto& decl : widget.models) {
        Entry entry{.kind = Entry::Kind::component, .name = decl.name, .type = decl.component, .decl = decl};
        entry.picked = picked_before(models, decl.name, decl.component);
        models_.push_back(std::move(entry));
    }
    // A controller's Models are its struct fields marked [[fj::model]].
    if (!widget.class_name.empty() && scripts_.is_loaded()) {
        if (const auto* controller = scripts_.find_ui_widget_type(widget.class_name)) {
            for (const auto& field : controller->fields) {
                if (!field.is_model() || field.struct_type == nullptr) continue;
                models_.push_back(owned_entry(field.name, *field.struct_type, models));
            }
        }
    }

    for (const auto& prop : widget.props) {
        if (is_value_type(prop.type)) {
            Entry entry{.kind = Entry::Kind::value, .name = prop.name, .type = prop.type};
            entry.value = UiValue::parse(prop.default_value, prop.type);
            for (const auto& old : props) {
                if (old.kind == Entry::Kind::value && old.name == prop.name && old.type == prop.type) {
                    entry.value = old.value;
                }
            }
            props_.push_back(std::move(entry));
        } else if (!scripts_.is_loaded()) {
            continue; // a reflected type, unknown until the game is built
        } else if (const auto* type = scripts_.reflected_types().find(prop.type)) {
            props_.push_back(owned_entry(prop.name, *type, props));
        } else if (scripts_.find_script_type(prop.type) != nullptr) {
            // A component prop has no rule of its own: the local player's
            // node stands in until one is picked.
            ModelDecl decl;
            decl.name = prop.name;
            decl.component = prop.type;
            decl.from = ModelSource::local;
            Entry entry{.kind = Entry::Kind::component, .name = prop.name, .type = prop.type, .decl = decl};
            entry.picked = picked_before(props, prop.name, prop.type);
            props_.push_back(std::move(entry));
        }
    }
}

// ── Into the preview ────────────────────────────────────────────────────

void UiPreviewData::fill(UiContext& root) {
    root_ = &root;
    for (auto* entries : {&models_, &props_}) {
        for (const auto& entry : *entries) {
            switch (entry.kind) {
                case Entry::Kind::value: root.set(entry.name, entry.value); break;
                case Entry::Kind::owned:
                    root.set(entry.name, entry.object ? UiValue(UiObject{entry.object->type().name,
                                                                         entry.object->type().fields,
                                                                         entry.object->get()})
                                                      : UiValue());
                    break;
                case Entry::Kind::component: root.set(entry.name, UiValue()); break;
            }
        }
    }
}

void UiPreviewData::attach(GameUi& ui) {
    looked_ = false;
    if (scene_ == nullptr) return;
    bind(ui); // finds the placing node and attaches
    auto& models = ui.models();
    for (auto* entries : {&models_, &props_}) {
        for (const auto& entry : *entries) {
            if (entry.kind == Entry::Kind::component) models.add(entry.decl, ui.context(), entry.picked);
        }
    }
    if (auto* root = ui.root()) models.add_widgets(*root);
    models.bind();
}

void UiPreviewData::bind(GameUi& ui) {
    if (scene_ == nullptr) return;
    // The node placing the widget is looked for again once nodes come or go.
    if (!looked_ || looked_scene_ != scene_->id() || looked_structure_ != scene_->structure_revision()) {
        looked_ = true;
        looked_scene_ = scene_->id();
        looked_structure_ = scene_->structure_revision();
        self_node_ = placing_node(*scene_, widget_path_, project_);
        ui.models().attach(*scene_, world_, self_node_);
    }
    ui.models().bind();
}

void UiPreviewData::release() {
    for (auto* entries : {&models_, &props_}) {
        for (auto& entry : *entries) {
            if (entry.kind == Entry::Kind::value) continue;
            if (root_ != nullptr) root_->set(entry.name, UiValue());
            entry.object.reset();
        }
    }
}

// ── The Data panel ──────────────────────────────────────────────────────

void UiPreviewData::draw(GameUi& ui, const InspectorContext& ctx) {
    if (models_.empty() && props_.empty()) {
        ftk::ui::empty_state(ftk::ui::icon::ui_models, "Nothing to feed", "This widget has no props or Models");
        return;
    }
    if (!scripts_.is_loaded()) ftk::ui::status(ftk::ui::Severity::Warning, "Scripts aren't built yet: Scripts > Build & Reload");

    if (!models_.empty()) {
        ftk::ui::section("Models", ftk::ui::icon::ui_models);
        for (auto& entry : models_) {
            ImGui::PushID(entry.name.c_str());
            if (entry.kind == Entry::Kind::component) draw_component(entry, ui, ctx);
            else draw_owned(entry, ctx);
            ImGui::PopID();
        }
    }
    if (!props_.empty()) {
        ftk::ui::section("Props", ftk::ui::icon::ui_props);
        draw_values(ui);
        for (auto& entry : props_) {
            ImGui::PushID(entry.name.c_str());
            if (entry.kind == Entry::Kind::component) draw_component(entry, ui, ctx);
            else if (entry.kind == Entry::Kind::owned) draw_owned(entry, ctx);
            ImGui::PopID();
        }
    }
}

void UiPreviewData::draw_component(Entry& entry, GameUi& ui, const InspectorContext& ctx) {
    ftk::ui::InsetHeading heading{.text = entry.name.c_str(),
                             .code = true,
                             .detail = entry.type.c_str(),
                             .live = scripts_.playing() ? "Live: the scene is playing" : nullptr};
    auto box = ftk::ui::InsetGroup("##component", heading);
    if (!box) return;

    const auto reading = ui.models().reading(ui.context(), entry.name);
    const bool self = entry.decl.from == ModelSource::self;
    if (auto table = ftk::ui::PropertyTable("##reads")) {
        ftk::ui::row("Reads", "The node the component is read from. Picked here, it is kept while the editor has the "
                         "widget open and never saved.",
                [&] {
                    uint32_t node = entry.picked;
                    const NodeSlotConfig slot{
                        .id = "##node",
                        .scene = scene_,
                        .refusal = [&](const Node& n) -> std::string {
                            return carries(n, entry.type) ? std::string() : n.name + " has no " + entry.type;
                        },
                        .empty_glyph = self ? ftk::ui::icon::this_node : ftk::ui::icon::local_player,
                        .empty_text = self ? "This node" : "Local player",
                    };
                    if (node_slot(slot, node)) {
                        entry.picked = node;
                        (void)ui.models().bind_node(ui.context(), entry.name, node);
                        ui.models().bind();
                    }
                });
        if (reading.component != nullptr && entry.picked == 0 && scene_ != nullptr) {
            if (const Node* found = scene_->find(reading.node)) ftk::ui::hint(("Found on " + found->name).c_str());
        }
    }
    if (reading.component == nullptr) {
        const std::string why = entry.picked != 0 ? "The picked node is gone or has no " + entry.type
                                                  : "No node in the scene has " + entry.type;
        ftk::ui::status(ftk::ui::Severity::Warning, why.c_str());
        return;
    }
    const auto type = world_.component_type(entry.type);
    if (!type) return;
    // Shown, never edited: the scene's values, which the game writes.
    (void)draw_model_fields(type->fields, const_cast<void*>(reading.component), // NOLINT(cppcoreguidelines-pro-type-const-cast)
                            {.path = entry.name, .entries_of = {}, .editable = false, .ctx = &ctx, .scene = scene_});
}

void UiPreviewData::draw_owned(Entry& entry, const InspectorContext& ctx) {
    bool reset = false;
    ftk::ui::InsetHeading heading{.text = entry.name.c_str(),
                             .code = true,
                             .detail = entry.type.c_str(),
                             .action_icon = ftk::ui::icon::revert,
                             .action_tooltip = "Back to the defaults",
                             .action = &reset};
    {
        auto box = ftk::ui::InsetGroup("##owned", heading);
        if (box && entry.object) {
            (void)draw_model_fields(entry.object->type().fields, entry.object->get(),
                                    {.path = entry.name, .entries_of = {}, .editable = true, .ctx = &ctx,
                                     .scene = scene_});
        }
    }
    if (reset && entry.object) entry.object->reset();
}

void UiPreviewData::draw_values(GameUi& ui) {
    const bool any = std::ranges::any_of(props_, [](const Entry& e) { return e.kind == Entry::Kind::value; });
    if (!any) return;
    auto table = ftk::ui::PropertyTable("##values");
    if (!table) return;
    for (auto& entry : props_) {
        if (entry.kind != Entry::Kind::value) continue;
        ImGui::PushID(entry.name.c_str());
        const std::string label = field_label(entry.name);
        ftk::ui::row(label.c_str(), entry.name.c_str(), [&] {
            bool changed = false;
            if (entry.type == "bool") {
                bool value = entry.value.truthy();
                if (ftk::ui::checkbox("##bool", value).changed) {
                    entry.value = UiValue(value);
                    changed = true;
                }
            } else if (entry.type == "int") {
                int value = static_cast<int>(entry.value.number().value_or(0.0f));
                if (ftk::ui::drag_int("##int", value).changed) {
                    entry.value = UiValue(value);
                    changed = true;
                }
            } else if (entry.type == "float") {
                float value = entry.value.number().value_or(0.0f);
                if (ftk::ui::drag("##float", value).changed) {
                    entry.value = UiValue(value);
                    changed = true;
                }
            } else {
                std::string value = entry.value.text();
                if (ftk::ui::text_field("##text", value).committed) {
                    entry.value = UiValue(value);
                    changed = true;
                }
            }
            if (changed) ui.context().set(entry.name, entry.value);
        });
        ImGui::PopID();
    }
}

} // namespace fjell
