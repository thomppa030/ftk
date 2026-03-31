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

#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>

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

    if (ImGui::Begin("Properties")) {
        draw_properties();
    }
    ImGui::End();

    update_preview(dt);
}

void BlendSpace2DEditorContext::draw_canvas() {
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    if (canvas_size.x < 50.0f) canvas_size.x = 50.0f;
    if (canvas_size.y < 50.0f) canvas_size.y = 50.0f;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Background
    draw_list->AddRectFilled(canvas_pos,
        {canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y},
        IM_COL32(30, 30, 30, 255));

    // Grid lines
    draw_list->AddLine(
        {canvas_pos.x + canvas_size.x * 0.5f, canvas_pos.y},
        {canvas_pos.x + canvas_size.x * 0.5f, canvas_pos.y + canvas_size.y},
        IM_COL32(60, 60, 60, 255));
    draw_list->AddLine(
        {canvas_pos.x, canvas_pos.y + canvas_size.y * 0.5f},
        {canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y * 0.5f},
        IM_COL32(60, 60, 60, 255));

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

    // Draw triangles
    for (const auto& tri : blend_space_.triangles()) {
        auto a = to_screen(blend_space_.points()[tri.a].position);
        auto b = to_screen(blend_space_.points()[tri.b].position);
        auto c = to_screen(blend_space_.points()[tri.c].position);
        draw_list->AddTriangle(a, b, c, IM_COL32(80, 80, 100, 120));
    }

    // Draw points
    for (uint32_t i = 0; i < blend_space_.size(); ++i) {
        auto screen = to_screen(blend_space_.points()[i].position);
        draw_list->AddCircleFilled(screen, 8.0f, IM_COL32(200, 160, 80, 255));
        draw_list->AddCircle(screen, 8.0f, IM_COL32(255, 255, 255, 180));

        // Label
        auto label = fs::path(blend_space_.points()[i].clip).stem().string();
        if (!label.empty()) {
            draw_list->AddText({screen.x + 10.0f, screen.y - 6.0f},
                               IM_COL32(200, 200, 200, 200), label.c_str());
        }
    }

    // Current parameter cursor
    auto cursor = to_screen(current_param_);
    draw_list->AddCircleFilled(cursor, 5.0f, IM_COL32(255, 200, 80, 255));

    // Invisible button for interaction
    ImGui::SetCursorScreenPos(canvas_pos);
    ImGui::InvisibleButton("##Canvas2D", canvas_size);

    // Drag parameter cursor
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        current_param_ = from_screen(ImGui::GetMousePos());
    }

    // Axis labels
    ImGui::TextDisabled("X: %s  Y: %s",
                        blend_space_.parameter_x().c_str(),
                        blend_space_.parameter_y().c_str());

    // Evaluation result
    if (!blend_space_.empty()) {
        auto result = blend_space_.evaluate(current_param_);
        if (result.count == 3) {
            ImGui::TextDisabled("Blend: [%u] %.0f%% + [%u] %.0f%% + [%u] %.0f%%",
                                result.indices[0], result.weights[0] * 100.0f,
                                result.indices[1], result.weights[1] * 100.0f,
                                result.indices[2], result.weights[2] * 100.0f);
        } else if (result.count == 2) {
            ImGui::TextDisabled("Blend: [%u] %.0f%% + [%u] %.0f%%",
                                result.indices[0], result.weights[0] * 100.0f,
                                result.indices[1], result.weights[1] * 100.0f);
        } else {
            ImGui::TextDisabled("Nearest: [%u]", result.indices[0]);
        }
    }
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
        changed = true;
    }
    ImGui::Text("Parameter Y");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##ParamY", py_buf, sizeof(py_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        blend_space_.set_parameters(blend_space_.parameter_x(), py_buf);
        changed = true;
    }

    ImGui::Spacing();
    ImGui::Separator();

    auto points = blend_space_.points(); // copy
    int remove_idx = -1;

    for (size_t i = 0; i < points.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        ImGui::Text("Position");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat2("##Pos", &points[i].position.x, 0.01f)) {
            changed = true;
        }

        char clip_buf[256];
        std::strncpy(clip_buf, points[i].clip.c_str(), sizeof(clip_buf) - 1);
        clip_buf[sizeof(clip_buf) - 1] = '\0';
        ImGui::Text("Clip");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##Clip", clip_buf, sizeof(clip_buf))) {
            points[i].clip = clip_buf;
            changed = true;
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                points[i].clip = std::string(static_cast<const char*>(payload->Data));
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            remove_idx = static_cast<int>(i);
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    if (remove_idx >= 0) {
        points.erase(points.begin() + remove_idx);
        changed = true;
    }

    if (ImGui::Button("+ Add Point", {-1, 0})) {
        BlendSpacePoint2D pt;
        pt.position = {0.0f, 0.0f};
        points.push_back(std::move(pt));
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
    ImGui::TextDisabled("File: %s", asset_path_.c_str());
    ImGui::TextDisabled("Points: %u", blend_space_.size());
    ImGui::TextDisabled("Triangles: %u", static_cast<uint32_t>(blend_space_.triangles().size()));
    ImGui::TextDisabled("Parameters: %s / %s",
                        blend_space_.parameter_x().c_str(),
                        blend_space_.parameter_y().c_str());
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

void BlendSpace2DEditorContext::update_preview(float /*dt*/) {
    // TODO: set up preview skeletal mesh with blend result playing
}

// ── Viewport boilerplate ────────────────────────────────────────────────

void BlendSpace2DEditorContext::setup_dockspace(ImGuiID main_area) {
    ImGuiID left, right;
    ImGui::DockBuilderSplitNode(main_area, ImGuiDir_Left, 0.25f, &left, &right);
    ImGuiID center, bottom;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.4f, &bottom, &center);

    ImGui::DockBuilderDockWindow("BS2D Preview", center);
    ImGui::DockBuilderDockWindow("Points", left);
    ImGui::DockBuilderDockWindow("Canvas", bottom);
    ImGui::DockBuilderDockWindow("Properties", left);
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
