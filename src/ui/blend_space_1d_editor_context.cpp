#include "ui/blend_space_1d_editor_context.hpp"
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

BlendSpace1DEditorContext::BlendSpace1DEditorContext(BlendSpace1DEditorDeps deps)
    : deps_{deps} {}

void BlendSpace1DEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "BS1D Preview");
}

void BlendSpace1DEditorContext::shutdown() {
}

void BlendSpace1DEditorContext::open(const std::string& path) {
    asset_path_ = path;
    auto resolved = base_path::resolve_asset(path, deps_.project_mgr->active());

    try {
        blend_space_ = BlendSpace1D::load(resolved);
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("BlendSpace1D: failed to load: {}", e.what());
        return;
    }

    name_ = fs::path(path).stem().string();
    reload_clips();

    FJELL_CORE_INFO("BlendSpace1D editor: opened {} ({} points, param='{}')",
                    path, blend_space_.size(), blend_space_.parameter());
}

void BlendSpace1DEditorContext::reload_clips() {
    loaded_clips_.clear();
    loaded_clips_.resize(blend_space_.size());

    for (uint32_t i = 0; i < blend_space_.size(); ++i) {
        const auto& pt = blend_space_.points()[i];
        if (pt.clip.empty()) continue;
        try {
            auto resolved = base_path::resolve_asset(pt.clip, deps_.project_mgr->active());
            loaded_clips_[i] = std::make_shared<AnimationClip>(AnimationClip::load(resolved));
        } catch (const std::exception& e) {
            FJELL_CORE_ERROR("BlendSpace1D: failed to load clip '{}': {}", pt.clip, e.what());
        }
    }
}

// ── Drawing ─────────────────────────────────────────────────────────────

void BlendSpace1DEditorContext::draw(float dt) {
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);

    // Parameter slider panel
    if (ImGui::Begin("Parameter")) {
        draw_parameter_slider();
    }
    ImGui::End();

    // Point list panel
    if (ImGui::Begin("Points")) {
        draw_point_list();
    }
    ImGui::End();

    // Properties
    if (ImGui::Begin("Properties")) {
        draw_properties();
    }
    ImGui::End();

    update_preview(dt);
}

void BlendSpace1DEditorContext::draw_parameter_slider() {
    ImGui::Text("Parameter: %s", blend_space_.parameter().c_str());

    float min_val = blend_space_.empty() ? 0.0f : blend_space_.points().front().position;
    float max_val = blend_space_.empty() ? 1.0f : blend_space_.points().back().position;
    if (min_val >= max_val) max_val = min_val + 1.0f;

    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##Param", &current_param_, min_val, max_val);

    // Visualize the 1D line with point markers
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    float canvas_width = ImGui::GetContentRegionAvail().x;
    float canvas_height = 40.0f;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Background bar
    draw_list->AddRectFilled(
        canvas_pos,
        {canvas_pos.x + canvas_width, canvas_pos.y + canvas_height},
        IM_COL32(40, 40, 40, 255));

    // Point markers
    float range = max_val - min_val;
    for (uint32_t i = 0; i < blend_space_.size(); ++i) {
        float t = (range > 0.0f) ? (blend_space_.points()[i].position - min_val) / range : 0.5f;
        float x = canvas_pos.x + t * canvas_width;
        draw_list->AddCircleFilled({x, canvas_pos.y + canvas_height * 0.5f}, 6.0f,
                                    IM_COL32(200, 160, 80, 255));
        draw_list->AddCircle({x, canvas_pos.y + canvas_height * 0.5f}, 6.0f,
                              IM_COL32(255, 255, 255, 180));
    }

    // Current param indicator
    float t = (range > 0.0f) ? (current_param_ - min_val) / range : 0.5f;
    float indicator_x = canvas_pos.x + t * canvas_width;
    draw_list->AddTriangleFilled(
        {indicator_x, canvas_pos.y},
        {indicator_x - 5.0f, canvas_pos.y - 8.0f},
        {indicator_x + 5.0f, canvas_pos.y - 8.0f},
        IM_COL32(255, 200, 80, 255));

    ImGui::Dummy({canvas_width, canvas_height});

    // Show evaluation result
    if (!blend_space_.empty()) {
        auto result = blend_space_.evaluate(current_param_);
        ImGui::TextDisabled("Blend: [%u] %.0f%% + [%u] %.0f%%",
                            result.index_a, result.weight_a * 100.0f,
                            result.index_b, result.weight_b * 100.0f);
    }
}

void BlendSpace1DEditorContext::draw_point_list() {
    bool changed = false;

    // Parameter name
    char param_buf[128];
    std::strncpy(param_buf, blend_space_.parameter().c_str(), sizeof(param_buf) - 1);
    param_buf[sizeof(param_buf) - 1] = '\0';
    ImGui::Text("Parameter Name");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##ParamName", param_buf, sizeof(param_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        blend_space_.set_parameter(param_buf);
        changed = true;
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Editable point list
    // We need to rebuild because BlendSpace1D sorts internally
    auto points = blend_space_.points(); // copy
    int remove_idx = -1;

    for (size_t i = 0; i < points.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        // Position
        ImGui::Text("Position");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##Pos", &points[i].position, 0.01f)) {
            changed = true;
        }

        // Clip path
        char clip_buf[256];
        std::strncpy(clip_buf, points[i].clip.c_str(), sizeof(clip_buf) - 1);
        clip_buf[sizeof(clip_buf) - 1] = '\0';
        ImGui::Text("Clip");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##Clip", clip_buf, sizeof(clip_buf))) {
            points[i].clip = clip_buf;
            changed = true;
        }
        // Drag-drop target
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
        BlendSpacePoint pt;
        pt.position = blend_space_.empty() ? 0.0f : blend_space_.points().back().position + 0.5f;
        points.push_back(std::move(pt));
        changed = true;
    }

    if (changed) {
        blend_space_.clear();
        for (auto& pt : points) {
            blend_space_.add(std::move(pt));
        }
        reload_clips();
    }
}

void BlendSpace1DEditorContext::draw_properties() {
    ImGui::TextDisabled("File: %s", asset_path_.c_str());
    ImGui::TextDisabled("Points: %u", blend_space_.size());
    ImGui::TextDisabled("Parameter: %s", blend_space_.parameter().c_str());
}

bool BlendSpace1DEditorContext::save() {
    if (asset_path_.empty()) return false;
    try {
        auto resolved = base_path::resolve_asset(asset_path_, deps_.project_mgr->active());
        blend_space_.save(resolved);
        FJELL_CORE_INFO("Saved blend space 1D: {}", asset_path_);
        return true;
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("Failed to save blend space 1D: {}", e.what());
        return false;
    }
}

void BlendSpace1DEditorContext::update_preview(float /*dt*/) {
    // TODO: set up preview skeletal mesh with blend result playing
}

// ── Viewport boilerplate ────────────────────────────────────────────────

void BlendSpace1DEditorContext::setup_dockspace(ImGuiID main_area) {
    ImGuiID left, right;
    ImGui::DockBuilderSplitNode(main_area, ImGuiDir_Left, 0.25f, &left, &right);
    ImGuiID center, bottom;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.3f, &bottom, &center);

    ImGui::DockBuilderDockWindow("BS1D Preview", center);
    ImGui::DockBuilderDockWindow("Points", left);
    ImGui::DockBuilderDockWindow("Parameter", bottom);
    ImGui::DockBuilderDockWindow("Properties", left);
}

bool BlendSpace1DEditorContext::handle_shortcuts() {
    return false;
}

std::vector<ViewportRenderRequest> BlendSpace1DEditorContext::build_render_requests() {
    return viewport_manager_.build_render_requests();
}

void BlendSpace1DEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

bool BlendSpace1DEditorContext::any_viewport_hovered() const {
    return viewport_manager_.any_hovered();
}

} // namespace fjell
