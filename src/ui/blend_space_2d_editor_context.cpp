#include "ui/blend_space_2d_editor_context.hpp"
#include "animation/animation_clip.hpp"
#include "animation/animator_component.hpp"
#include "animation/animation_subsystem.hpp"
#include "animation/skeleton.hpp"
#include "core/base_path.hpp"
#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/skeletal_mesh_component.hpp"
#include "renderer/skeletal_model.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace fjell {

BlendSpace2DEditorContext::BlendSpace2DEditorContext(BlendSpace2DEditorDeps deps)
    : deps_{deps} {}

void BlendSpace2DEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "BS2D Preview");
}

void BlendSpace2DEditorContext::shutdown() {
}

void BlendSpace2DEditorContext::open(const std::string& path) {
    asset_path_ = path;
    auto resolved = base_path::resolve_asset(path, deps_.project_mgr->active());

    try {
        blend_space_ = BlendSpace2D::load(resolved);
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("BlendSpace2D: failed to load: {}", e.what());
        return;
    }

    name_ = fs::path(path).stem().string();
    reload_clips();

    // Load skeleton if stored in the blend space
    if (!blend_space_.skeleton_path().empty()) {
        load_skeleton(blend_space_.skeleton_path());
    } else {
        scan_available_clips();
    }

    FJELL_CORE_INFO("BlendSpace2D editor: opened {} ({} points, params='{}/{}', {} triangles)",
                    path, blend_space_.size(), blend_space_.parameter_x(),
                    blend_space_.parameter_y(),
                    static_cast<uint32_t>(blend_space_.triangles().size()));
}

void BlendSpace2DEditorContext::reload_clips() {
    loaded_clips_.clear();
    loaded_clips_.resize(blend_space_.size());

    for (uint32_t i = 0; i < blend_space_.size(); ++i) {
        const auto& pt = blend_space_.points()[i];
        if (pt.clip.empty()) continue;
        try {
            auto resolved = base_path::resolve_asset(pt.clip, deps_.project_mgr->active());
            loaded_clips_[i] = std::make_shared<AnimationClip>(AnimationClip::load(resolved));
        } catch (const std::exception& e) {
            FJELL_CORE_ERROR("BlendSpace2D: failed to load clip '{}': {}", pt.clip, e.what());
        }
    }

    // Re-sync preview state machine when clips change
    if (preview_node_id_ != 0 && skeleton_) {
        setup_preview_state_machine();
    }
}

// ── Drawing ─────────────────────────────────────────────────────────────

void BlendSpace2DEditorContext::draw(float dt) {
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);

    if (ImGui::Begin("Canvas")) {
        draw_canvas();
    }
    ImGui::End();

    if (ImGui::Begin("Points")) {
        draw_point_list();
    }
    ImGui::End();

    if (ImGui::Begin("Clips")) {
        draw_clip_list();
    }
    ImGui::End();

    if (ImGui::Begin("Properties")) {
        draw_properties();
    }
    ImGui::End();

    update_preview(dt);
}

void BlendSpace2DEditorContext::draw_canvas() {
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    // Reserve space for the info text below the canvas
    canvas_size.y -= ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    if (canvas_size.x < 50.0f) canvas_size.x = 50.0f;
    if (canvas_size.y < 50.0f) canvas_size.y = 50.0f;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    constexpr float point_radius = 8.0f;
    constexpr float hit_radius = 14.0f;

    // Background
    draw_list->AddRectFilled(canvas_pos,
        {canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y},
        IM_COL32(25, 26, 28, 255));
    draw_list->AddRect(canvas_pos,
        {canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y},
        IM_COL32(50, 50, 55, 255));

    // Map parameter space to canvas (-1..1 default range, auto-fit to points)
    float min_x = -1.0f, max_x = 1.0f, min_y = -1.0f, max_y = 1.0f;
    for (const auto& pt : blend_space_.points()) {
        min_x = std::min(min_x, pt.position.x - 0.5f);
        max_x = std::max(max_x, pt.position.x + 0.5f);
        min_y = std::min(min_y, pt.position.y - 0.5f);
        max_y = std::max(max_y, pt.position.y + 0.5f);
    }

    auto to_screen = [&](glm::vec2 p) -> ImVec2 {
        float sx = (p.x - min_x) / (max_x - min_x) * canvas_size.x + canvas_pos.x;
        float sy = (1.0f - (p.y - min_y) / (max_y - min_y)) * canvas_size.y + canvas_pos.y;
        return {sx, sy};
    };

    auto from_screen = [&](ImVec2 s) -> glm::vec2 {
        float px = (s.x - canvas_pos.x) / canvas_size.x * (max_x - min_x) + min_x;
        float py = (1.0f - (s.y - canvas_pos.y) / canvas_size.y) * (max_y - min_y) + min_y;
        return {px, py};
    };

    // Grid lines — minor grid
    constexpr int grid_divs = 8;
    for (int i = 1; i < grid_divs; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(grid_divs);
        float gx = canvas_pos.x + t * canvas_size.x;
        float gy = canvas_pos.y + t * canvas_size.y;
        draw_list->AddLine({gx, canvas_pos.y}, {gx, canvas_pos.y + canvas_size.y},
                           IM_COL32(40, 40, 44, 255));
        draw_list->AddLine({canvas_pos.x, gy}, {canvas_pos.x + canvas_size.x, gy},
                           IM_COL32(40, 40, 44, 255));
    }

    // Center crosshair (origin lines)
    auto origin = to_screen({0.0f, 0.0f});
    if (origin.x > canvas_pos.x && origin.x < canvas_pos.x + canvas_size.x) {
        draw_list->AddLine({origin.x, canvas_pos.y}, {origin.x, canvas_pos.y + canvas_size.y},
                           IM_COL32(70, 70, 75, 255));
    }
    if (origin.y > canvas_pos.y && origin.y < canvas_pos.y + canvas_size.y) {
        draw_list->AddLine({canvas_pos.x, origin.y}, {canvas_pos.x + canvas_size.x, origin.y},
                           IM_COL32(70, 70, 75, 255));
    }

    // Axis labels at edges
    char x_label[64], y_label[64];
    std::snprintf(x_label, sizeof(x_label), "%s ->", blend_space_.parameter_x().c_str());
    std::snprintf(y_label, sizeof(y_label), "%s", blend_space_.parameter_y().c_str());
    draw_list->AddText({canvas_pos.x + canvas_size.x - ImGui::CalcTextSize(x_label).x - 4.0f,
                        canvas_pos.y + canvas_size.y - ImGui::GetTextLineHeight() - 2.0f},
                       IM_COL32(100, 100, 110, 180), x_label);
    draw_list->AddText({canvas_pos.x + 4.0f, canvas_pos.y + 2.0f},
                       IM_COL32(100, 100, 110, 180), y_label);

    // Draw triangles (filled + wireframe)
    for (const auto& tri : blend_space_.triangles()) {
        auto a = to_screen(blend_space_.points()[tri.a].position);
        auto b = to_screen(blend_space_.points()[tri.b].position);
        auto c = to_screen(blend_space_.points()[tri.c].position);
        draw_list->AddTriangleFilled(a, b, c, IM_COL32(60, 60, 80, 40));
        draw_list->AddTriangle(a, b, c, IM_COL32(80, 80, 110, 100));
    }

    // Hit test: find hovered point
    ImVec2 mouse = ImGui::GetMousePos();
    hovered_point_ = -1;
    for (uint32_t i = 0; i < blend_space_.size(); ++i) {
        auto screen = to_screen(blend_space_.points()[i].position);
        float dx = mouse.x - screen.x;
        float dy = mouse.y - screen.y;
        if (dx * dx + dy * dy <= hit_radius * hit_radius) {
            hovered_point_ = static_cast<int>(i);
            break;
        }
    }

    // Draw points
    for (uint32_t i = 0; i < blend_space_.size(); ++i) {
        auto screen = to_screen(blend_space_.points()[i].position);
        int idx = static_cast<int>(i);

        // Fill color
        ImU32 fill = IM_COL32(200, 160, 80, 255);
        if (idx == selected_point_) {
            fill = IM_COL32(255, 200, 80, 255);
        } else if (idx == hovered_point_) {
            fill = IM_COL32(230, 180, 60, 255);
        }

        draw_list->AddCircleFilled(screen, point_radius, fill);

        // Outline
        ImU32 outline = IM_COL32(180, 180, 180, 160);
        if (idx == selected_point_) {
            outline = IM_COL32(255, 255, 255, 255);
        } else if (idx == hovered_point_) {
            outline = IM_COL32(255, 255, 255, 220);
        }
        draw_list->AddCircle(screen, point_radius, outline, 0, 2.0f);

        // Label
        auto label = fs::path(blend_space_.points()[i].clip).stem().string();
        if (label.empty()) label = "?";
        draw_list->AddText({screen.x + 11.0f, screen.y - 6.0f},
                           IM_COL32(200, 200, 200, 200), label.c_str());
    }

    // Draw cursor (the preview position indicator)
    auto cursor_screen = to_screen(current_param_);
    // Diamond shape
    constexpr float d = 6.0f;
    draw_list->AddQuadFilled(
        {cursor_screen.x, cursor_screen.y - d},
        {cursor_screen.x + d, cursor_screen.y},
        {cursor_screen.x, cursor_screen.y + d},
        {cursor_screen.x - d, cursor_screen.y},
        IM_COL32(80, 220, 120, 220));
    draw_list->AddQuad(
        {cursor_screen.x, cursor_screen.y - d},
        {cursor_screen.x + d, cursor_screen.y},
        {cursor_screen.x, cursor_screen.y + d},
        {cursor_screen.x - d, cursor_screen.y},
        IM_COL32(255, 255, 255, 180));

    // Invisible button for interaction
    ImGui::SetCursorScreenPos(canvas_pos);
    ImGui::InvisibleButton("##Canvas2D", canvas_size);
    bool canvas_active = ImGui::IsItemActive();
    bool canvas_hovered = ImGui::IsItemHovered();

    // ── Mouse interaction ───────────────────────────────────────────────

    // Left click/drag: pick up a point or move the cursor
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hovered_point_ >= 0) {
            // Start dragging a point
            dragging_point_ = hovered_point_;
            selected_point_ = hovered_point_;
            dragging_cursor_ = false;
        } else {
            // Start dragging the cursor
            dragging_cursor_ = true;
            dragging_point_ = -1;
            selected_point_ = -1;
            current_param_ = from_screen(ImGui::GetMousePos());
        }
    }

    if (canvas_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (dragging_point_ >= 0 && dragging_point_ < static_cast<int>(blend_space_.size())) {
            // Move the point
            auto new_pos = from_screen(ImGui::GetMousePos());
            auto points = blend_space_.points(); // copy
            points[dragging_point_].position = new_pos;
            blend_space_.clear();
            for (auto& pt : points) {
                blend_space_.add(std::move(pt));
            }
            blend_space_.triangulate();
        } else if (dragging_cursor_) {
            current_param_ = from_screen(ImGui::GetMousePos());
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        dragging_point_ = -1;
        dragging_cursor_ = false;
    }

    // Double-click on empty space: add new point
    if (canvas_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hovered_point_ < 0) {
        BlendSpacePoint2D pt;
        pt.position = from_screen(ImGui::GetMousePos());
        blend_space_.add(std::move(pt));
        blend_space_.triangulate();
        selected_point_ = static_cast<int>(blend_space_.size()) - 1;
        reload_clips();
    }

    // Right-click on a point: delete it
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hovered_point_ >= 0) {
        auto points = blend_space_.points(); // copy
        points.erase(points.begin() + hovered_point_);
        blend_space_.clear();
        for (auto& pt : points) {
            blend_space_.add(std::move(pt));
        }
        blend_space_.triangulate();
        if (selected_point_ == hovered_point_) selected_point_ = -1;
        else if (selected_point_ > hovered_point_) selected_point_--;
        hovered_point_ = -1;
        reload_clips();
    }

    // Delete key on selected point
    if (selected_point_ >= 0 && selected_point_ < static_cast<int>(blend_space_.size()) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        auto points = blend_space_.points();
        points.erase(points.begin() + selected_point_);
        blend_space_.clear();
        for (auto& pt : points) {
            blend_space_.add(std::move(pt));
        }
        blend_space_.triangulate();
        selected_point_ = -1;
        reload_clips();
    }

    // Drag-drop .fjanim onto canvas: onto a point reassigns, onto empty space creates
    if (ImGui::BeginDragDropTarget()) {
        if (auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            auto clip_path = std::string(static_cast<const char*>(payload->Data),
                                         static_cast<size_t>(payload->DataSize - 1));
            if (hovered_point_ >= 0) {
                // Reassign clip on existing point
                auto points = blend_space_.points();
                points[hovered_point_].clip = clip_path;
                blend_space_.clear();
                for (auto& pt : points) {
                    blend_space_.add(std::move(pt));
                }
            } else {
                // Create new point at drop position
                BlendSpacePoint2D pt;
                pt.clip = clip_path;
                pt.position = from_screen(ImGui::GetMousePos());
                blend_space_.add(std::move(pt));
                selected_point_ = static_cast<int>(blend_space_.size()) - 1;
            }
            blend_space_.triangulate();
            reload_clips();
        }
        ImGui::EndDragDropTarget();
    }

    // ── Info text below canvas ──────────────────────────────────────────

    ImGui::SetCursorScreenPos({canvas_pos.x, canvas_pos.y + canvas_size.y + 2.0f});

    if (!blend_space_.empty()) {
        auto result = blend_space_.evaluate(current_param_);
        if (result.count == 3) {
            auto n0 = fs::path(blend_space_.points()[result.indices[0]].clip).stem().string();
            auto n1 = fs::path(blend_space_.points()[result.indices[1]].clip).stem().string();
            auto n2 = fs::path(blend_space_.points()[result.indices[2]].clip).stem().string();
            ImGui::TextDisabled("Blend: %s %.0f%%  %s %.0f%%  %s %.0f%%",
                                n0.c_str(), result.weights[0] * 100.0f,
                                n1.c_str(), result.weights[1] * 100.0f,
                                n2.c_str(), result.weights[2] * 100.0f);
        } else if (result.count == 2) {
            auto n0 = fs::path(blend_space_.points()[result.indices[0]].clip).stem().string();
            auto n1 = fs::path(blend_space_.points()[result.indices[1]].clip).stem().string();
            ImGui::TextDisabled("Blend: %s %.0f%%  %s %.0f%%",
                                n0.c_str(), result.weights[0] * 100.0f,
                                n1.c_str(), result.weights[1] * 100.0f);
        } else {
            auto n0 = fs::path(blend_space_.points()[result.indices[0]].clip).stem().string();
            ImGui::TextDisabled("Nearest: %s", n0.c_str());
        }
    } else {
        ImGui::TextDisabled("Drop clips to place, double-click for empty, drag to move");
    }

    // Cursor coordinates
    ImGui::TextDisabled("Cursor: %.2f, %.2f", current_param_.x, current_param_.y);
}

void BlendSpace2DEditorContext::draw_point_list() {
    // Parameter names
    char px_buf[128], py_buf[128];
    std::strncpy(px_buf, blend_space_.parameter_x().c_str(), sizeof(px_buf) - 1);
    std::strncpy(py_buf, blend_space_.parameter_y().c_str(), sizeof(py_buf) - 1);
    px_buf[sizeof(px_buf) - 1] = '\0';
    py_buf[sizeof(py_buf) - 1] = '\0';

    bool changed = false;

    ImGui::Text("Parameter X");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##ParamX", px_buf, sizeof(px_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        blend_space_.set_parameters(px_buf, blend_space_.parameter_y());
    }
    ImGui::Text("Parameter Y");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##ParamY", py_buf, sizeof(py_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        blend_space_.set_parameters(blend_space_.parameter_x(), py_buf);
    }

    ImGui::Spacing();
    ImGui::Separator();

    auto points = blend_space_.points(); // copy
    int remove_idx = -1;

    for (size_t i = 0; i < points.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        bool is_selected = (static_cast<int>(i) == selected_point_);

        // Selectable header with clip name
        auto label = fs::path(points[i].clip).stem().string();
        if (label.empty()) label = "(no clip)";
        if (ImGui::Selectable(label.c_str(), is_selected)) {
            selected_point_ = static_cast<int>(i);
        }

        // Position
        ImGui::SetNextItemWidth(-30);
        if (ImGui::DragFloat2("##Pos", &points[i].position.x, 0.01f)) {
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            remove_idx = static_cast<int>(i);
        }

        // Clip drag-drop
        char clip_buf[256];
        std::strncpy(clip_buf, points[i].clip.c_str(), sizeof(clip_buf) - 1);
        clip_buf[sizeof(clip_buf) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##Clip", clip_buf, sizeof(clip_buf))) {
            points[i].clip = clip_buf;
            changed = true;
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                points[i].clip = std::string(static_cast<const char*>(payload->Data),
                                             static_cast<size_t>(payload->DataSize - 1));
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    if (remove_idx >= 0) {
        points.erase(points.begin() + remove_idx);
        if (selected_point_ == remove_idx) selected_point_ = -1;
        else if (selected_point_ > remove_idx) selected_point_--;
        changed = true;
    }

    if (ImGui::Button("+ Add Point", {-1, 0})) {
        BlendSpacePoint2D pt;
        pt.position = {0.0f, 0.0f};
        points.push_back(std::move(pt));
        selected_point_ = static_cast<int>(points.size()) - 1;
        changed = true;
    }

    if (changed) {
        blend_space_.clear();
        for (auto& pt : points) {
            blend_space_.add(std::move(pt));
        }
        blend_space_.triangulate();
        reload_clips();
    }
}

void BlendSpace2DEditorContext::draw_properties() {
    // Skeleton assignment
    ImGui::SeparatorText("Skeleton");
    auto skel = blend_space_.skeleton_path();
    if (skel.empty()) {
        ImGui::TextDisabled("None — drag .fjskel here");
    } else {
        ImGui::Text("%s", fs::path(skel).filename().string().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##skel")) {
            blend_space_.set_skeleton_path("");
            available_clip_paths_.clear();
            available_clip_names_.clear();
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            std::string path(static_cast<const char*>(payload->Data),
                             static_cast<size_t>(payload->DataSize - 1));
            if (path.ends_with(".fjskel")) {
                load_skeleton(path);
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::Separator();
    ImGui::TextDisabled("File: %s", asset_path_.c_str());
    ImGui::TextDisabled("Points: %u", blend_space_.size());
    ImGui::TextDisabled("Triangles: %u", static_cast<uint32_t>(blend_space_.triangles().size()));
    ImGui::TextDisabled("Parameters: %s / %s",
                        blend_space_.parameter_x().c_str(),
                        blend_space_.parameter_y().c_str());
}

void BlendSpace2DEditorContext::draw_clip_list() {
    if (available_clip_names_.empty()) {
        ImGui::TextDisabled("Assign a skeleton to see clips");
        return;
    }

    for (int i = 0; i < static_cast<int>(available_clip_names_.size()); ++i) {
        ImGui::PushID(i);

        ImGui::Selectable(available_clip_names_[i].c_str(), false);

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            const auto& path = available_clip_paths_[i];
            ImGui::SetDragDropPayload("ASSET_PATH", path.c_str(), path.size() + 1);
            ImGui::Text("%s", available_clip_names_[i].c_str());
            ImGui::EndDragDropSource();
        }

        ImGui::PopID();
    }
}

void BlendSpace2DEditorContext::load_skeleton(const std::string& skel_path) {
    blend_space_.set_skeleton_path(skel_path);
    scan_available_clips();

    // Set up preview scene with skeletal mesh
    auto resolved = base_path::resolve_asset(skel_path, deps_.project_mgr->active());
    auto model_result = deps_.resource_manager->load_skeletal_model(resolved);
    if (!model_result) {
        FJELL_CORE_ERROR("BS2D: failed to load skeletal model: {}", model_result.error());
        return;
    }

    auto* skel_model = deps_.resource_manager->get(*model_result);
    if (!skel_model || !skel_model->skeleton()) return;

    skeleton_ = skel_model->skeleton_shared();

    preview_scene_.clear();
    auto node = std::make_unique<SceneNode>();
    node->name = "bs2d_preview";

    auto& smc = node->components.add<SkeletalMeshComponent>();
    smc.skeletal_model_handle = *model_result;
    smc.skeletal_model = skel_model;

    auto tex = deps_.resource_manager->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    smc.material_handle = deps_.resource_manager->create_material("bs2d_preview", tex);
    smc.material = deps_.resource_manager->get(smc.material_handle);

    auto& anim = node->components.add<AnimatorComponent>();
    anim.init(skel_model->skeleton());

    preview_node_id_ = preview_scene_.add_node(std::move(node));

    if (auto* cam = viewport_manager_.active_camera()) {
        cam->focus({0.0f, 1.0f, 0.0f}, 2.5f);
    }

    setup_preview_state_machine();
}

void BlendSpace2DEditorContext::scan_available_clips() {
    available_clip_paths_.clear();
    available_clip_names_.clear();

    auto skel = blend_space_.skeleton_path();
    if (skel.empty()) return;

    auto resolved = base_path::resolve_asset(skel, deps_.project_mgr->active());
    auto dir = fs::path(resolved).parent_path();
    auto base = base_path::resolve_asset("", deps_.project_mgr->active());

    if (!fs::is_directory(dir)) return;

    std::vector<std::pair<std::string, std::string>> pairs;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".fjanim") {
            auto rel = fs::relative(entry.path(), base).string();
            auto name = entry.path().stem().string();
            pairs.emplace_back(name, rel);
        }
    }
    std::sort(pairs.begin(), pairs.end());
    for (auto& [name, path] : pairs) {
        available_clip_names_.push_back(std::move(name));
        available_clip_paths_.push_back(std::move(path));
    }
}

bool BlendSpace2DEditorContext::save() {
    if (asset_path_.empty()) return false;
    try {
        auto resolved = base_path::resolve_asset(asset_path_, deps_.project_mgr->active());
        blend_space_.save(resolved);
        FJELL_CORE_INFO("Saved blend space 2D: {}", asset_path_);
        return true;
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("Failed to save blend space 2D: {}", e.what());
        return false;
    }
}

void BlendSpace2DEditorContext::setup_preview_state_machine() {
    if (preview_node_id_ == 0 || blend_space_.empty() || !skeleton_) return;

    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* anim = node->components.get<AnimatorComponent>();
    if (!anim) return;

    // Register all clips from the blend space
    for (uint32_t i = 0; i < blend_space_.size(); ++i) {
        if (i < loaded_clips_.size() && loaded_clips_[i]) {
            anim->set_clip(blend_space_.points()[i].clip, loaded_clips_[i].get());
        }
    }

    // Register the blend space itself
    anim->load_blend_space_2d(asset_path_, blend_space_);

    // Build a minimal state machine with one blend space state
    // We write a temp JSON and load it so the state machine is properly initialized
    nlohmann::json j;
    j["initial"] = "Preview";
    j["params"] = {{blend_space_.parameter_x(), 0.0f},
                    {blend_space_.parameter_y(), 0.0f}};
    j["states"] = nlohmann::json::array();
    j["states"].push_back({
        {"name", "Preview"},
        {"blend_space", asset_path_},
        {"loop", true}
    });

    // Write to temp string and load
    auto tmp = std::filesystem::temp_directory_path() / "fjell_bs2d_preview.fjanimset";
    {
        std::ofstream f(tmp);
        f << j.dump(2);
    }
    anim->load_state_machine(tmp.string());
    std::filesystem::remove(tmp);
}

void BlendSpace2DEditorContext::update_preview(float dt) {
    if (preview_node_id_ == 0 || blend_space_.empty()) return;

    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* anim = node->components.get<AnimatorComponent>();
    if (!anim || !anim->initialized()) return;

    // Forward cursor position as parameters
    anim->set_param(blend_space_.parameter_x(), current_param_.x);
    anim->set_param(blend_space_.parameter_y(), current_param_.y);

    preview_scene_.compute_world_matrices(nullptr);
    auto& system = deps_.animation_subsystem->system();
    system.update(preview_scene_, dt);
}

// ── Viewport boilerplate ────────────────────────────────────────────────

std::vector<std::string> BlendSpace2DEditorContext::docked_window_names() const {
    return {"Canvas", "BS2D Preview", "Points", "Clips", "Properties"};
}

void BlendSpace2DEditorContext::setup_dockspace(ImGuiID main_area) {
    ImGuiID left, center;
    ImGui::DockBuilderSplitNode(main_area, ImGuiDir_Left, 0.22f, &left, &center);
    ImGuiID left_mid, left_bottom;
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.60f, &left_mid, &left);
    ImGui::DockBuilderSplitNode(left_mid, ImGuiDir_Down, 0.40f, &left_bottom, &left_mid);

    ImGui::DockBuilderDockWindow("Canvas", center);
    ImGui::DockBuilderDockWindow("BS2D Preview", center);
    ImGui::DockBuilderDockWindow("Points", left);
    ImGui::DockBuilderDockWindow("Clips", left_mid);
    ImGui::DockBuilderDockWindow("Properties", left_bottom);
}

bool BlendSpace2DEditorContext::handle_shortcuts() {
    return false;
}

std::vector<ViewportRenderRequest> BlendSpace2DEditorContext::build_render_requests() {
    return viewport_manager_.build_render_requests();
}

void BlendSpace2DEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

bool BlendSpace2DEditorContext::any_viewport_hovered() const {
    return viewport_manager_.any_hovered();
}

} // namespace fjell
