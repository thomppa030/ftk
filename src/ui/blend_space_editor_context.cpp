#include "ui/blend_space_editor_context.hpp"

#include "animation/animation_clip.hpp"
#include "animation/animation_subsystem.hpp"
#include "animation/animator_component.hpp"
#include "animation/skeleton.hpp"
#include "core/base_path.hpp"
#include "core/project_manager.hpp"
#include "ftk/base/log.hpp"
#include "ftk/ui/kit/canvas.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/list_editor.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ftk/ui/kit/tree.hpp"
#include "ftk/ui/theme.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace fjell {

namespace {

// The canvas's metrics (sheet 8): a handle, the 1D canvas's height and the
// cursor's size.
constexpr float HANDLE_RADIUS = 5.0f;
constexpr float LINE_HEIGHT = 94.0f;
constexpr float CURSOR = 6.0f;

ImU32 colour(const ImVec4& c) { return ImGui::GetColorU32(c); }

std::string clip_name(const std::string& path) {
    const std::string stem = fs::path(path).stem().string();
    return stem.empty() ? std::string("No clip") : stem;
}

} // namespace

BlendSpaceEditorContext::BlendSpaceEditorContext(BlendSpaceEditorDeps deps) : deps_{deps} {}

EditorContext::DockPreset BlendSpaceEditorContext::dock_preset() const {
    return {.tree = {"Points", "Clips"}, .preview = {"Preview"}, .under_preview = {"Canvas"},
            .inspector = {"Properties"}};
}

void BlendSpaceEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Preview");
    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

void BlendSpaceEditorContext::open(const std::string& path) {
    asset_path_ = path;
    name_ = fs::path(path).stem().string();
    const auto resolved = base_path::resolve_asset(path, deps_.project_mgr->active());
    if (path.ends_with(".fjblend2D")) {
        auto loaded = BlendSpace2D::load(resolved);
        if (!loaded) {
            FJELL_CORE_ERROR("Blend space editor: {}", loaded.error());
            return;
        }
        blend_space_ = std::move(*loaded);
    } else {
        auto loaded = BlendSpace1D::load(resolved);
        if (!loaded) {
            FJELL_CORE_ERROR("Blend space editor: {}", loaded.error());
            return;
        }
        blend_space_ = std::move(*loaded);
    }
    view_ = two_d() ? ui::CanvasView({.zoom = ui::CanvasZoom::Both, .y_up = true})
                    : ui::CanvasView({.zoom = ui::CanvasZoom::Horizontal});
    frame_pending_ = !view_.recall("blend_space|" + path);
    reload_clips();
    const std::string skeleton = std::visit([](const auto& bs) { return bs.skeleton_path(); }, blend_space_);
    if (!skeleton.empty()) {
        load_skeleton(skeleton);
    } else {
        scan_available_clips();
    }
    document_opened();
}

// ── Points ───────────────────────────────────────────────────────────────

std::size_t BlendSpaceEditorContext::point_count() const {
    return std::visit([](const auto& bs) -> std::size_t { return bs.size(); }, blend_space_);
}

const std::string& BlendSpaceEditorContext::point_clip(std::size_t i) const {
    return std::visit([i](const auto& bs) -> const std::string& { return bs.points()[i].clip; }, blend_space_);
}

glm::vec2 BlendSpaceEditorContext::point_position(std::size_t i) const {
    if (const auto* one = std::get_if<BlendSpace1D>(&blend_space_)) return {one->points()[i].position, 0.0f};
    return std::get<BlendSpace2D>(blend_space_).points()[i].position;
}

int BlendSpaceEditorContext::set_point(std::size_t i, const std::string& clip, glm::vec2 position) {
    if (auto* one = std::get_if<BlendSpace1D>(&blend_space_)) {
        // Kept in order: rebuilt, then the moved point found again.
        auto points = one->points();
        points[i] = {clip, position.x};
        one->clear();
        for (auto& p : points) one->add(std::move(p));
        for (std::size_t k = 0; k < one->size(); ++k) {
            if (one->points()[k].clip == clip && one->points()[k].position == position.x) return static_cast<int>(k);
        }
        return static_cast<int>(i);
    }
    auto& two = std::get<BlendSpace2D>(blend_space_);
    auto points = two.points();
    points[i] = {clip, position};
    two.clear();
    for (auto& p : points) two.add(std::move(p));
    two.triangulate();
    return static_cast<int>(i);
}

int BlendSpaceEditorContext::add_point(const std::string& clip, glm::vec2 position) {
    if (auto* one = std::get_if<BlendSpace1D>(&blend_space_)) {
        one->add({clip, position.x});
        for (std::size_t k = 0; k < one->size(); ++k) {
            if (one->points()[k].clip == clip && one->points()[k].position == position.x) return static_cast<int>(k);
        }
        return static_cast<int>(one->size()) - 1;
    }
    auto& two = std::get<BlendSpace2D>(blend_space_);
    two.add({clip, position});
    two.triangulate();
    return static_cast<int>(two.size()) - 1;
}

void BlendSpaceEditorContext::remove_point(std::size_t i) {
    std::visit([i](auto& bs) {
        auto points = bs.points();
        points.erase(points.begin() + static_cast<std::ptrdiff_t>(i));
        bs.clear();
        for (auto& p : points) bs.add(std::move(p));
        if constexpr (std::is_same_v<std::decay_t<decltype(bs)>, BlendSpace2D>) bs.triangulate();
    }, blend_space_);
    if (selected_point_ == static_cast<int>(i)) selected_point_ = -1;
    else if (selected_point_ > static_cast<int>(i)) --selected_point_;
    hovered_point_ = -1;
}

std::vector<std::pair<std::size_t, float>> BlendSpaceEditorContext::blend_at_cursor() const {
    std::vector<std::pair<std::size_t, float>> blend;
    if (point_count() == 0) return blend;
    if (const auto* one = std::get_if<BlendSpace1D>(&blend_space_)) {
        const auto r = one->evaluate(cursor_.x);
        blend.emplace_back(r.index_a, r.weight_a);
        if (r.index_b != r.index_a && r.weight_b > 0.0f) blend.emplace_back(r.index_b, r.weight_b);
    } else {
        const auto r = std::get<BlendSpace2D>(blend_space_).evaluate(cursor_);
        for (uint32_t k = 0; k < r.count; ++k) {
            if (r.weights[k] > 0.0f) blend.emplace_back(r.indices[k], r.weights[k]);
        }
    }
    std::sort(blend.begin(), blend.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return blend;
}

void BlendSpaceEditorContext::points_changed() {
    reload_clips();
}

// ── Panels ───────────────────────────────────────────────────────────────

void BlendSpaceEditorContext::draw(float dt) {
    name_preview(viewport_manager_);
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    draw_canvas_panel();
    draw_points_panel();
    draw_clips_panel();
    draw_properties_panel();
    update_preview(dt);
    draw_shared_panels(dt);
}

void BlendSpaceEditorContext::draw_canvas_panel() {
    auto panel = ui::Panel(ui::icon::blend_space, ctx_title("Canvas").c_str());
    if (!panel) return;

    // What the cursor blends, over the canvas.
    if (auto t = ui::PropertyTable("##blend")) {
        const auto blend = blend_at_cursor();
        std::string text;
        for (const auto& [index, weight] : blend) {
            char part[96];
            std::snprintf(part, sizeof(part), "%s%s %.0f%%", text.empty() ? "" : "  \xc2\xb7  ",
                          clip_name(point_clip(index)).c_str(), weight * 100.0f);
            text += part;
        }
        ui::row("Blend", "What the preview plays at the cursor: drag on the canvas to move it.",
                [&] { ui::readout(text.empty() ? "Nothing: no points yet" : text.c_str()); });
    }

    const ImVec2 outer = ImGui::GetCursorScreenPos();
    ImVec2 outer_size = ImGui::GetContentRegionAvail();
    outer_size.x = std::max(outer_size.x, 80.0f);
    outer_size.y = two_d() ? std::max(outer_size.y, 80.0f) : LINE_HEIGHT;
    ImVec2 origin;
    ImVec2 size;
    ui::plot_area(outer, outer_size, plot_axes(), &origin, &size);
    view_.place(origin, size);
    if (frame_pending_) {
        frame_points();
        frame_pending_ = false;
    }
    ui::canvas_plot_axes(ImGui::GetWindowDrawList(), view_, plot_axes());
    if (two_d()) {
        draw_canvas_2d(origin, size);
    } else {
        draw_canvas_1d(origin, size);
    }
    ImGui::SetCursorScreenPos({outer.x, outer.y + outer_size.y});
    ImGui::Dummy({outer_size.x, 0.0f});
    if (point_count() == 0) {
        ui::hint("Drop a clip on the canvas, or double-click it for an empty point");
    }
}

ui::PlotAxes BlendSpaceEditorContext::plot_axes() const {
    if (const auto* bs = std::get_if<BlendSpace2D>(&blend_space_)) {
        return {.x_name = bs->parameter_x().c_str(), .y_name = bs->parameter_y().c_str()};
    }
    return {.x_name = std::get<BlendSpace1D>(blend_space_).parameter().c_str(), .y_axis = false};
}

void BlendSpaceEditorContext::frame_points() {
    glm::vec2 lo{-1.0f};
    glm::vec2 hi{1.0f};
    for (std::size_t i = 0; i < point_count(); ++i) {
        lo = glm::min(lo, point_position(i) - 0.5f);
        hi = glm::max(hi, point_position(i) + 0.5f);
    }
    view_.frame(lo, hi);
}

void BlendSpaceEditorContext::draw_canvas_1d(ImVec2 origin, ImVec2 size) {
    const float axis_y = origin.y + size.y * 0.5f;
    const auto to_screen = [&](glm::vec2 p) { return ImVec2{view_.to_screen_x(p.x), axis_y}; };
    const auto from_screen = [&](ImVec2 s) { return glm::vec2{view_.to_world_x(s.x), 0.0f}; };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine({origin.x, std::floor(axis_y) + 0.5f}, {origin.x + size.x, std::floor(axis_y) + 0.5f},
                colour(theme::grid_major()));

    canvas_input(origin, size, to_screen, from_screen);
}

void BlendSpaceEditorContext::draw_canvas_2d(ImVec2 origin, ImVec2 size) {
    const auto& bs = std::get<BlendSpace2D>(blend_space_);
    const auto to_screen = [&](glm::vec2 p) { return view_.to_screen(p); };
    const auto from_screen = [&](ImVec2 s) { return view_.to_world(s); };
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The triangles the blend is read from, in the animation colour.
    ImVec4 fill = theme::category(theme::Category::Animation);
    fill.w = 0.10f;
    ImVec4 edge = theme::category(theme::Category::Animation);
    edge.w = 0.45f;
    for (const auto& tri : bs.triangles()) {
        const ImVec2 a = to_screen(bs.points()[tri.a].position);
        const ImVec2 b = to_screen(bs.points()[tri.b].position);
        const ImVec2 c = to_screen(bs.points()[tri.c].position);
        dl->AddTriangleFilled(a, b, c, colour(fill));
        dl->AddTriangle(a, b, c, colour(edge));
    }

    canvas_input(origin, size, to_screen, from_screen);
}

template <typename ToScreen, typename FromScreen>
void BlendSpaceEditorContext::canvas_input(ImVec2 origin, ImVec2 size, ToScreen&& to_screen,
                                           FromScreen&& from_screen) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    // Hovered while a clip is dragged in from Clips too, so a drop can land
    // on a point.
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool active = ImGui::IsItemActive();
    view_.input(ImGui::IsItemHovered());
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    hovered_point_ = -1;
    if (hovered) {
        for (std::size_t i = 0; i < point_count(); ++i) {
            const ImVec2 p = to_screen(point_position(i));
            const float dx = mouse.x - p.x;
            const float dy = mouse.y - p.y;
            if (dx * dx + dy * dy <= ui::CANVAS_HIT_RADIUS * ui::CANVAS_HIT_RADIUS) hovered_point_ = static_cast<int>(i);
        }
    }

    // A clip dropped on a point gives it the clip; on empty space, a new point.
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            const std::string clip(static_cast<const char*>(payload->Data),
                                   static_cast<std::size_t>(payload->DataSize - 1));
            if (clip.ends_with(".fjanim")) {
                if (hovered_point_ >= 0) {
                    selected_point_ = set_point(static_cast<std::size_t>(hovered_point_), clip,
                                                point_position(static_cast<std::size_t>(hovered_point_)));
                } else {
                    selected_point_ = add_point(clip, from_screen(mouse));
                }
                points_changed();
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Pressing picks a point to drag, or moves the cursor there.
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !view_.panning()) {
        if (hovered_point_ >= 0) {
            dragging_point_ = hovered_point_;
            selected_point_ = hovered_point_;
        } else {
            dragging_cursor_ = true;
            cursor_ = from_screen(mouse);
        }
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        if (dragging_point_ >= 0 && dragging_point_ < static_cast<int>(point_count())) {
            const std::string clip = point_clip(static_cast<std::size_t>(dragging_point_));
            dragging_point_ = set_point(static_cast<std::size_t>(dragging_point_), clip, from_screen(mouse));
            selected_point_ = dragging_point_;
        } else if (dragging_cursor_) {
            cursor_ = from_screen(mouse);
        }
    }
    if (ImGui::IsItemDeactivated()) {
        if (dragging_point_ >= 0) points_changed();
        dragging_point_ = -1;
        dragging_cursor_ = false;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hovered_point_ < 0 && !view_.panning()) {
        selected_point_ = add_point({}, from_screen(mouse));
        points_changed();
    }

    // Right-click offers what a double-click and Del do.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hovered_point_ >= 0) selected_point_ = hovered_point_;
        menu_position_ = from_screen(mouse);
        ImGui::OpenPopup(hovered_point_ >= 0 ? "##point_menu" : "##canvas_menu");
    }
    if (ImGui::BeginPopup("##canvas_menu")) {
        if (ui::menu_item({.icon = ui::icon::add, .label = "Add point here"})) {
            selected_point_ = add_point({}, menu_position_);
            points_changed();
        }
        if (ui::menu_item({.label = "Move the cursor here"})) cursor_ = menu_position_;
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##point_menu")) {
        if (ui::menu_item({.icon = ui::icon::remove, .label = "Remove point", .shortcut = "Del", .destructive = true}) &&
            selected_point_ >= 0) {
            remove_point(static_cast<std::size_t>(selected_point_));
            points_changed();
        }
        ImGui::EndPopup();
    }
    if (selected_point_ >= 0 && selected_point_ < static_cast<int>(point_count()) && ui::canvas_has_keys() &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        remove_point(static_cast<std::size_t>(selected_point_));
        points_changed();
    }
    if (ui::canvas_has_keys() && ImGui::IsKeyPressed(ImGuiKey_F, false)) frame_points();

    // Points: handles at rest in the secondary text, brighter with a ring
    // under the mouse, the selected one in the accent, each named.
    dl->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    for (std::size_t i = 0; i < point_count(); ++i) {
        const ImVec2 p = to_screen(point_position(i));
        const bool sel = static_cast<int>(i) == selected_point_;
        const bool hot = static_cast<int>(i) == hovered_point_;
        ui::canvas_handle(dl, p, hot, sel, {.radius = HANDLE_RADIUS});
        const std::string name = clip_name(point_clip(i));
        const ImVec2 text = ImGui::CalcTextSize(name.c_str());
        const ImVec2 at = two_d() ? ImVec2{p.x + HANDLE_RADIUS + 5.0f, p.y - text.y * 0.5f}
                                  : ImVec2{p.x - text.x * 0.5f, p.y - HANDLE_RADIUS - text.y - 6.0f};
        dl->AddText(at, colour(sel ? theme::text() : theme::text_secondary()), name.c_str());
    }
    // While a point moves, where it is reads beside it.
    if (dragging_point_ >= 0 && dragging_point_ < static_cast<int>(point_count())) {
        const glm::vec2 at = point_position(static_cast<std::size_t>(dragging_point_));
        char readout[48];
        if (two_d()) {
            std::snprintf(readout, sizeof(readout), "%.2f, %.2f", at.x, at.y);
        } else {
            std::snprintf(readout, sizeof(readout), "%.2f", at.x);
        }
        ui::canvas_readout(dl, to_screen(at), readout, origin, {origin.x + size.x, origin.y + size.y});
    }
    // The cursor: what the preview plays, in the playhead's white.
    const ImVec2 c = two_d() ? to_screen(cursor_) : ImVec2{to_screen(cursor_).x, to_screen(cursor_).y + 20.0f};
    dl->AddQuadFilled({c.x, c.y - CURSOR}, {c.x + CURSOR, c.y}, {c.x, c.y + CURSOR}, {c.x - CURSOR, c.y},
                      colour(theme::text()));
    if (!two_d()) {
        dl->AddLine({std::floor(c.x) + 0.5f, origin.y}, {std::floor(c.x) + 0.5f, c.y - CURSOR}, colour(theme::text()));
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos({origin.x, origin.y + size.y});
    ImGui::Dummy({size.x, 0.0f});
}

void BlendSpaceEditorContext::draw_points_panel() {
    auto panel = ui::Panel(ui::icon::blend_space, ctx_title("Points").c_str());
    if (!panel) return;
    InspectorContext ictx{};
    ictx.resource_manager = deps_.resource_manager;
    ictx.project_mgr = deps_.project_mgr;
    ictx.icons = deps_.icon_cache;

    bool changed = false;
    std::optional<std::size_t> remove;
    bool add = false;
    {
        ui::ListEditor list("##points", point_count(), {.reorder = false, .opens = true});
        for (std::size_t i = 0; i < point_count(); ++i) {
            const bool open = selected_point_ == static_cast<int>(i);
            if (!list.begin_item(i, open)) continue;
            const glm::vec2 position = point_position(i);
            if (open) {
                if (auto t = ui::PropertyTable("##point")) {
                    ui::row("Position", [&] {
                        glm::vec2 moved = position;
                        const ui::Edit edit = two_d() ? ui::vec2("##position", moved, {.speed = 0.01f})
                                                      : ui::drag("##position", moved.x, {.speed = 0.01f});
                        if (edit.changed) {
                            selected_point_ = set_point(i, point_clip(i), moved);
                        }
                        changed |= edit.committed;
                    });
                    ui::row("Clip", [&] {
                        static const std::string clip_exts[] = {".fjanim"};
                        std::string clip = point_clip(i);
                        if (asset_slot({.label = "Clip", .id = "##clip", .extensions = clip_exts, .ctx = &ictx}, clip)) {
                            selected_point_ = set_point(i, clip, position);
                            changed = true;
                        }
                    });
                }
            } else {
                char summary[160];
                if (two_d()) {
                    std::snprintf(summary, sizeof(summary), "%s \xc2\xb7 %.2f, %.2f", clip_name(point_clip(i)).c_str(),
                                  position.x, position.y);
                } else {
                    std::snprintf(summary, sizeof(summary), "%s \xc2\xb7 %.2f", clip_name(point_clip(i)).c_str(),
                                  position.x);
                }
                ImGui::TextUnformatted(summary);
            }
            if (list.end_item()) remove = i;
            if (list.clicked()) selected_point_ = open ? -1 : static_cast<int>(i);
        }
        if (point_count() == 0) list.empty("No points: each places a clip on the canvas");
        add = list.add_button("Add point");
    }
    if (add) {
        glm::vec2 at{0.0f, 0.0f};
        if (!two_d() && point_count() > 0) at.x = point_position(point_count() - 1).x + 0.5f;
        selected_point_ = add_point({}, at);
        changed = true;
    }
    if (remove) {
        remove_point(*remove);
        changed = true;
    }
    if (changed) points_changed();
}

void BlendSpaceEditorContext::draw_clips_panel() {
    auto panel = ui::Panel(ui::icon::animation, ctx_title("Clips").c_str());
    if (!panel) return;
    if (available_clip_names_.empty()) {
        ui::empty_state(ui::icon::animation, "No clips", "Pick the skeleton in Properties to list its clips");
        return;
    }
    // Dragged onto the canvas, a clip becomes a point, or a point's clip.
    if (auto tree = ui::Tree("##clips")) {
        for (std::size_t i = 0; i < available_clip_names_.size(); ++i) {
            const std::string id = std::to_string(i);
            (void)ui::tree_row({.id = id.c_str(), .name = available_clip_names_[i], .icon = ui::icon::animation,
                                .category = theme::Category::Animation});
            if (ImGui::BeginDragDropSource()) {
                const auto& path = available_clip_paths_[i];
                ImGui::SetDragDropPayload("ASSET_PATH", path.c_str(), path.size() + 1);
                ui::drag_preview(ui::icon::animation, theme::Category::Animation, available_clip_names_[i]);
                ImGui::EndDragDropSource();
            }
        }
    }
}

void BlendSpaceEditorContext::draw_properties_panel() {
    auto panel = ui::Panel(ui::icon::properties, ctx_title("Properties").c_str());
    if (!panel) return;
    InspectorContext ictx{};
    ictx.resource_manager = deps_.resource_manager;
    ictx.project_mgr = deps_.project_mgr;
    ictx.icons = deps_.icon_cache;

    ui::section("Blend space", ui::icon::blend_space);
    if (auto t = ui::PropertyTable("##blend_space")) {
        ui::row("Kind", [&] { ui::readout(two_d() ? "Two parameters" : "One parameter"); });
        if (auto* one = std::get_if<BlendSpace1D>(&blend_space_)) {
            ui::row("Parameter", "The animator parameter the position is read from, e.g. speed.", [&] {
                std::string name = one->parameter();
                if (ui::text_field("##parameter", name).committed) one->set_parameter(name);
            });
        } else {
            auto& two = std::get<BlendSpace2D>(blend_space_);
            ui::row("Parameter X", "The animator parameter along the canvas's width.", [&] {
                std::string name = two.parameter_x();
                if (ui::text_field("##parameter_x", name).committed) two.set_parameters(name, two.parameter_y());
            });
            ui::row("Parameter Y", "The animator parameter up the canvas.", [&] {
                std::string name = two.parameter_y();
                if (ui::text_field("##parameter_y", name).committed) two.set_parameters(two.parameter_x(), name);
            });
        }
        ui::row("Skeleton", "The skeleton the clips are made for; its clips are listed in Clips.", [&] {
            static const std::string skel_exts[] = {".fjskel"};
            std::string skeleton = std::visit([](const auto& bs) { return bs.skeleton_path(); }, blend_space_);
            if (!asset_slot({.label = "Skeleton", .id = "##skeleton", .extensions = skel_exts, .ctx = &ictx}, skeleton)) {
                return;
            }
            if (skeleton.empty()) {
                std::visit([](auto& bs) { bs.set_skeleton_path({}); }, blend_space_);
                available_clip_paths_.clear();
                available_clip_names_.clear();
            } else {
                load_skeleton(skeleton);
            }
        });
        char count[32];
        std::snprintf(count, sizeof(count), "%zu", point_count());
        ui::row("Points", [&] { ui::readout(count); });
        if (const auto* two = std::get_if<BlendSpace2D>(&blend_space_)) {
            char triangles[32];
            std::snprintf(triangles, sizeof(triangles), "%zu", two->triangles().size());
            ui::row("Triangles", "The blend reads the three points around the cursor.", [&] { ui::readout(triangles); });
        }
    }
}

// ── Preview ──────────────────────────────────────────────────────────────

void BlendSpaceEditorContext::reload_clips() {
    loaded_clips_.clear();
    loaded_clips_.resize(point_count());
    for (std::size_t i = 0; i < point_count(); ++i) {
        const std::string& clip = point_clip(i);
        if (clip.empty()) continue;
        auto loaded = AnimationClip::load(base_path::resolve_asset(clip, deps_.project_mgr->active()));
        if (loaded) {
            loaded_clips_[i] = std::make_shared<AnimationClip>(std::move(*loaded));
        } else {
            FJELL_CORE_ERROR("Blend space editor: clip {}: {}", clip, loaded.error());
        }
    }
    if (preview_node_id_ != 0 && skeleton_) setup_preview_state_machine();
}

void BlendSpaceEditorContext::load_skeleton(const std::string& skel_path) {
    std::visit([&](auto& bs) { bs.set_skeleton_path(skel_path); }, blend_space_);
    scan_available_clips();

    auto model = deps_.resource_manager->load_skeletal_model(base_path::resolve_asset(skel_path, deps_.project_mgr->active()));
    if (!model) {
        FJELL_CORE_ERROR("Blend space editor: {}", model.error());
        return;
    }
    auto* skel_model = deps_.resource_manager->get(*model);
    if (!skel_model || !skel_model->skeleton()) return;
    skeleton_ = skel_model->skeleton_shared();

    preview_scene_.clear();
    auto node = std::make_unique<SceneNode>();
    node->name = "blend_space_preview";
    auto& smc = node->components.add<SkeletalMeshComponent>();
    smc.skeletal_model_handle = *model;
    smc.skeletal_model = skel_model;
    auto tex = deps_.resource_manager->load_texture(base_path::resolve("engine_assets/textures/test.png"))
                   .value_or(TextureHandle{});
    smc.material_handle = deps_.resource_manager->create_material("blend_space_preview", tex);
    smc.material = deps_.resource_manager->get(smc.material_handle);
    auto& anim = node->components.add<AnimatorComponent>();
    anim.init(skel_model->skeleton());
    preview_node_id_ = preview_scene_.add_node(std::move(node));
    viewport_manager_.frame({0.0f, 1.0f, 0.0f}, 2.5f);

    setup_preview_state_machine();
}

void BlendSpaceEditorContext::setup_preview_state_machine() {
    if (preview_node_id_ == 0 || point_count() == 0 || !skeleton_) return;
    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* anim = node->components.get<AnimatorComponent>();
    if (!anim) return;

    for (std::size_t i = 0; i < point_count(); ++i) {
        if (i < loaded_clips_.size() && loaded_clips_[i]) anim->set_clip(point_clip(i), loaded_clips_[i].get());
    }

    // A one-state machine playing this blend space, fed the cursor.
    nlohmann::json params = nlohmann::json::object();
    if (const auto* one = std::get_if<BlendSpace1D>(&blend_space_)) {
        anim->load_blend_space_1d(asset_path_, *one);
        params[one->parameter()] = 0.0f;
    } else {
        const auto& two = std::get<BlendSpace2D>(blend_space_);
        anim->load_blend_space_2d(asset_path_, two);
        params[two.parameter_x()] = 0.0f;
        params[two.parameter_y()] = 0.0f;
    }
    nlohmann::json j;
    j["initial"] = "Preview";
    j["params"] = params;
    j["states"] = nlohmann::json::array({{{"name", "Preview"}, {"blend_space", asset_path_}, {"loop", true}}});

    const auto tmp = fs::temp_directory_path() / "fjell_blend_space_preview.fjanimset";
    {
        std::ofstream f(tmp);
        f << j.dump(2);
    }
    if (auto r = anim->load_state_machine(tmp.string()); !r) {
        FJELL_CORE_WARN("Blend space editor: preview state machine: {}", r.error());
    }
    fs::remove(tmp);
}

void BlendSpaceEditorContext::scan_available_clips() {
    available_clip_paths_.clear();
    available_clip_names_.clear();
    const std::string skeleton = std::visit([](const auto& bs) { return bs.skeleton_path(); }, blend_space_);
    if (skeleton.empty()) return;

    const auto dir = fs::path(base_path::resolve_asset(skeleton, deps_.project_mgr->active())).parent_path();
    const auto base = base_path::resolve_asset("", deps_.project_mgr->active());
    if (!fs::is_directory(dir)) return;

    std::vector<std::pair<std::string, std::string>> pairs;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".fjanim") {
            pairs.emplace_back(entry.path().stem().string(), fs::relative(entry.path(), base).string());
        }
    }
    std::sort(pairs.begin(), pairs.end());
    for (auto& [name, path] : pairs) {
        available_clip_names_.push_back(std::move(name));
        available_clip_paths_.push_back(std::move(path));
    }
}

void BlendSpaceEditorContext::update_preview(float dt) {
    if (preview_node_id_ == 0 || point_count() == 0) return;
    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* anim = node->components.get<AnimatorComponent>();
    if (!anim || !anim->initialized()) return;

    if (const auto* one = std::get_if<BlendSpace1D>(&blend_space_)) {
        anim->set_param(one->parameter(), cursor_.x);
    } else {
        const auto& two = std::get<BlendSpace2D>(blend_space_);
        anim->set_param(two.parameter_x(), cursor_.x);
        anim->set_param(two.parameter_y(), cursor_.y);
    }
    preview_scene_.compute_world_matrices(nullptr);
    deps_.animation_subsystem->system().update(preview_scene_, dt);
}

// ── Document ─────────────────────────────────────────────────────────────

std::string BlendSpaceEditorContext::document() const {
    if (asset_path_.empty()) return {};
    return std::visit([](const auto& bs) { return bs.text(); }, blend_space_);
}

Result<> BlendSpaceEditorContext::write() {
    if (asset_path_.empty()) return make_error("no blend space is open");
    const auto resolved = base_path::resolve_asset(asset_path_, deps_.project_mgr->active());
    return std::visit([&](const auto& bs) { return bs.save(resolved); }, blend_space_);
}

Result<> BlendSpaceEditorContext::restore(const std::string& text) {
    const std::string before = std::visit([](const auto& bs) { return bs.skeleton_path(); }, blend_space_);
    if (two_d()) {
        auto restored = BlendSpace2D::from_text(text, asset_path_);
        if (!restored) return make_error(restored.error());
        blend_space_ = std::move(*restored);
    } else {
        auto restored = BlendSpace1D::from_text(text, asset_path_);
        if (!restored) return make_error(restored.error());
        blend_space_ = std::move(*restored);
    }
    if (selected_point_ >= static_cast<int>(point_count())) selected_point_ = -1;
    reload_clips();
    const std::string skeleton = std::visit([](const auto& bs) { return bs.skeleton_path(); }, blend_space_);
    if (skeleton != before) {
        if (!skeleton.empty()) {
            load_skeleton(skeleton);
        } else {
            scan_available_clips();
        }
    }
    return {};
}

std::vector<ViewportRenderRequest> BlendSpaceEditorContext::build_render_requests() {
    return preview_render_requests(viewport_manager_.build_render_requests());
}

void BlendSpaceEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

} // namespace fjell
