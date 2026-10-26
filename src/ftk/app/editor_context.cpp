#include "ftk/app/editor_context.hpp"
#include "ftk/app/console.hpp"
#include "ftk/ui/kit/asset_header.hpp"
#include "ftk/ui/kit/asset_kind.hpp"
#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/edit_record.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/base/log.hpp"

#include <filesystem>

namespace ftk {

void EditorContext::draw_shared_panels(float /*dt*/) {
    auto history_title = ctx_title("History");
    auto console_title = ctx_title("Console");

    history_panel_.draw(history_title.c_str());
    if (auto* sink = console_sink()) {
        sink->draw(console_title.c_str());
    }
}

namespace {

// How long has_unsaved_changes() reuses its answer, in seconds.
constexpr double UNSAVED_RECHECK = 0.25;

// Whether this frame could have finished an edit made without holding an
// item: a click released, a key pressed (Delete, Enter), a drop.
bool input_finished_something() {
    return ui::any_mouse_released() || ui::any_key_pressed();
}

} // namespace

void EditorContext::draw_frame(float dt) {
    const std::string scope = "context/" + std::to_string(context_id_);
    ui::edit_scope(scope);
    draw(dt);
    ui::edit_scope({});

    const bool editing = ui::any_item_active();
    const bool finished = !editing && input_finished_something();
    std::string what = std::string("Edit ") + name();
    if (finished || !editing) {
        if (auto record = ui::take_edit_record(scope)) {
            what = describe_change(record->label, record->before, record->after);
        }
    }
    document_history_.frame(editing, finished, what);
}

Result<> EditorContext::restore_document(const std::string& text) {
    auto restored = restore(text);
    unsaved_checked_at_ = -1.0;
    return restored;
}

Result<> EditorContext::save() {
    if (auto written = write(); !written) {
        save_error_ = written.error();
        return written;
    }
    save_error_.clear();
    saved_document_ = document();
    command_history_.mark_saved();
    unsaved_ = false;
    unsaved_checked_at_ = ui::seconds();
    return {};
}

bool EditorContext::has_unsaved_changes() const {
    const double now = ui::seconds();
    if (unsaved_checked_at_ < 0.0 || now - unsaved_checked_at_ >= UNSAVED_RECHECK) {
        unsaved_ = document() != saved_document_;
        unsaved_checked_at_ = now;
    }
    return unsaved_;
}

void EditorContext::set_saved_document(std::string text) {
    saved_document_ = std::move(text);
    unsaved_checked_at_ = -1.0;
}

void EditorContext::take_document(const std::string& text, const std::string& what) {
    document_history_.change_to(text, what);
    unsaved_checked_at_ = -1.0;
}

void EditorContext::document_opened() {
    saved_document_ = document();
    document_history_.opened();
    command_history_.mark_saved();
    unsaved_ = false;
    unsaved_checked_at_ = -1.0;
}

void EditorContext::revert() {
    if (!has_unsaved_changes()) return;
    document_history_.change_to(saved_document_, "Revert to saved");
}

std::string EditorContext::saved_files() const {
    return std::filesystem::path(asset_path()).filename().string();
}

std::string EditorContext::header_name() const {
    return std::filesystem::path(asset_path()).filename().string();
}

void EditorContext::draw_header() {
    const std::filesystem::path path = asset_path();
    if (path.empty()) return;

    const auto& kind = ui::asset_kind(path.extension().string());
    std::string folder;
    for (const auto& part : path.parent_path()) {
        if (!folder.empty()) folder += " / ";
        folder += part.string();
    }
    const ui::AssetHeaderSpec spec{
        .icon = kind.icon,
        .icon_colour = ui::kind_colour(kind),
        .name = header_name(),
        .name_tooltip = header_tooltip(),
        .folder = std::move(folder),
        .unsaved = has_unsaved_changes(),
        .saves = saved_files(),
        .error = save_error_,
    };
    auto header = ui::AssetHeaderBar(spec);
    if (!header) return;
    header.begin_actions();
    draw_header_actions();
    switch (header.finish()) {
        case ui::SaveAction::Save:
            if (auto saved = save(); !saved) {
                FTK_CORE_ERROR("Couldn't save {}: {}", name(), saved.error());
            }
            break;
        case ui::SaveAction::Revert: revert(); break;
        case ui::SaveAction::None: break;
    }
}

void EditorContext::draw_dockspace(bool shown) {
    dockspace_id_ = ui::id("CtxDock" + std::to_string(context_id_));
    ui::dockspace(dockspace_id_, {.see_through = true, .hidden = !shown});
}

bool EditorContext::has_keyboard() const {
    return ui::keyboard_in_dockspace(dockspace_id_);
}

std::vector<std::string> EditorContext::bottom_tabs() const {
    return {"Console", "History"};
}

std::vector<std::string> EditorContext::docked_window_names() const {
    const DockPreset preset = dock_preset();
    std::vector<std::string> names;
    for (const auto* slot : {&preset.above_tree, &preset.tree, &preset.preview, &preset.under_preview, &preset.inspector}) {
        for (const auto& name : *slot) names.push_back(ctx_title(name.c_str()));
    }
    for (const auto& name : bottom_tabs()) names.push_back(ctx_title(name.c_str()));
    return names;
}

void EditorContext::setup_dockspace(ui::Id main_area) {
    const DockPreset preset = dock_preset();
    // Columns first (tree, inspector), then the rows of the middle one:
    // the bottom tabs, then what sits under the preview. Each split leaves
    // the middle with the rest.
    ui::Id centre = main_area;
    ui::Id tree = 0;
    ui::Id above_tree = 0;
    ui::Id inspector = 0;
    ui::Id under = 0;
    const auto split = [&](ui::DockSide side, float ratio) {
        const ui::DockSplit parts = ui::split_dock(centre, side, ratio);
        centre = parts.rest;
        return parts.side;
    };
    if (!preset.tree.empty()) {
        tree = split(ui::DockSide::Left, 0.22f);
        if (!preset.above_tree.empty()) {
            const ui::DockSplit parts = ui::split_dock(tree, ui::DockSide::Top, 0.4f);
            above_tree = parts.side;
            tree = parts.rest;
        }
    }
    if (!preset.inspector.empty()) {
        const float share = preset.tree.empty() ? 0.29f : 0.33f;
        inspector = split(ui::DockSide::Right, share);
    }
    const ui::Id bottom = split(ui::DockSide::Bottom, 0.27f);
    if (!preset.under_preview.empty()) {
        under = split(ui::DockSide::Bottom, 0.38f);
    }

    auto dock = [&](const std::vector<std::string>& names, ui::Id node) {
        for (const auto& name : names) ui::dock_window(ctx_title(name.c_str()), node);
    };
    dock(preset.above_tree, above_tree);
    dock(preset.tree, tree);
    dock(preset.preview, centre);
    dock(preset.under_preview, under);
    dock(preset.inspector, inspector);
    dock(bottom_tabs(), bottom);
}

} // namespace ftk
