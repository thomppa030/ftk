#include "ui/blend_space_1d_editor_context.hpp"
#include "ui/dockspace_layout.hpp"
#include "animation/animation_clip.hpp"
#include "animation/animator_component.hpp"
#include "animation/animation_subsystem.hpp"
#include "animation/skeleton.hpp"
#include "core/base_path.hpp"
#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace fjell {

BlendSpace1DEditorContext::BlendSpace1DEditorContext(BlendSpace1DEditorDeps deps)
    : deps_{deps} {}

void BlendSpace1DEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "BS1D Preview");

    init_shared_panels(deps_.vulkan_context, deps_.project_mgr);
    history_panel().init(&command_history());
}

void BlendSpace1DEditorContext::shutdown() {
}

void BlendSpace1DEditorContext::open(const std::string& path) {
    asset_path_ = path;
    auto resolved = base_path::resolve_asset(path, deps_.project_mgr->active());

    auto bs_result = BlendSpace1D::load(resolved);
    if (!bs_result) {
        FJELL_CORE_ERROR("BlendSpace1D: failed to load: {}", bs_result.error());
        return;
    }
    blend_space_ = std::move(*bs_result);

    name_ = fs::path(path).stem().string();
    reload_clips();

    if (!blend_space_.skeleton_path().empty()) {
        load_skeleton(blend_space_.skeleton_path());
    } else {
        scan_available_clips();
    }

    FJELL_CORE_INFO("BlendSpace1D editor: opened {} ({} points, param='{}')",
                    path, blend_space_.size(), blend_space_.parameter());
}

void BlendSpace1DEditorContext::reload_clips() {
    loaded_clips_.clear();
    loaded_clips_.resize(blend_space_.size());

    for (uint32_t i = 0; i < blend_space_.size(); ++i) {
        const auto& pt = blend_space_.points()[i];
        if (pt.clip.empty()) continue;
        auto resolved = base_path::resolve_asset(pt.clip, deps_.project_mgr->active());
        auto clip_result = AnimationClip::load(resolved);
        if (clip_result) {
            loaded_clips_[i] = std::make_shared<AnimationClip>(std::move(*clip_result));
        } else {
            FJELL_CORE_ERROR("BlendSpace1D: failed to load clip '{}': {}", pt.clip, clip_result.error());
        }
    }

    if (preview_node_id_ != 0 && skeleton_) {
        setup_preview_state_machine();
    }
}

// ── Drawing ─────────────────────────────────────────────────────────────

void BlendSpace1DEditorContext::draw(float dt) {
    if (auto* vp = viewport_manager_.active_panel()) {
        auto t = ctx_title("BS1D Preview");
        if (vp->title != t) vp->title = std::move(t);
    }
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);

    if (ImGui::Begin(ctx_title("Parameter").c_str())) {
        draw_parameter_slider();
    }
    ImGui::End();

    if (ImGui::Begin(ctx_title("Points").c_str())) {
        draw_point_list();
    }
    ImGui::End();

    if (ImGui::Begin(ctx_title("Clips").c_str())) {
        draw_clip_list();
    }
    ImGui::End();

    if (ImGui::Begin(ctx_title("Properties").c_str())) {
        draw_properties();
    }
    ImGui::End();

    update_preview(dt);

    draw_shared_panels(dt);
}

void BlendSpace1DEditorContext::draw_parameter_slider() {
    constexpr float point_radius = 8.0f;
    constexpr float hit_radius = 14.0f;
    constexpr float canvas_height = 60.0f;

    float min_val = blend_space_.empty() ? -1.0f : blend_space_.points().front().position - 0.5f;
    float max_val = blend_space_.empty() ? 1.0f : blend_space_.points().back().position + 0.5f;
    if (min_val >= max_val) max_val = min_val + 1.0f;
    float range = max_val - min_val;

    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    float canvas_width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    float cy = canvas_pos.y + canvas_height * 0.5f;

    auto to_screen_x = [&](float val) -> float {
        return canvas_pos.x + (val - min_val) / range * canvas_width;
    };
    auto from_screen_x = [&](float sx) -> float {
        return (sx - canvas_pos.x) / canvas_width * range + min_val;
    };

    // Background
    draw_list->AddRectFilled(canvas_pos,
        {canvas_pos.x + canvas_width, canvas_pos.y + canvas_height},
        IM_COL32(25, 26, 28, 255));
    draw_list->AddRect(canvas_pos,
        {canvas_pos.x + canvas_width, canvas_pos.y + canvas_height},
        IM_COL32(50, 50, 55, 255));

    // Horizontal axis line
    draw_list->AddLine({canvas_pos.x, cy}, {canvas_pos.x + canvas_width, cy},
                       IM_COL32(60, 60, 65, 255));

    // Tick marks
    constexpr int ticks = 10;
    for (int i = 0; i <= ticks; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(ticks);
        float tx = canvas_pos.x + t * canvas_width;
        draw_list->AddLine({tx, cy - 3.0f}, {tx, cy + 3.0f}, IM_COL32(60, 60, 65, 255));
    }

    // Axis label
    draw_list->AddText({canvas_pos.x + canvas_width - ImGui::CalcTextSize(blend_space_.parameter().c_str()).x - 4.0f,
                        canvas_pos.y + 2.0f},
                       IM_COL32(100, 100, 110, 180), blend_space_.parameter().c_str());

    // Hit test: find hovered point
    ImVec2 mouse = ImGui::GetMousePos();
    hovered_point_ = -1;
    for (uint32_t i = 0; i < blend_space_.size(); ++i) {
        float px = to_screen_x(blend_space_.points()[i].position);
        float dx = mouse.x - px;
        float dy = mouse.y - cy;
        if (dx * dx + dy * dy <= hit_radius * hit_radius) {
            hovered_point_ = static_cast<int>(i);
            break;
        }
    }

    // Draw points
    for (uint32_t i = 0; i < blend_space_.size(); ++i) {
        float px = to_screen_x(blend_space_.points()[i].position);
        int idx = static_cast<int>(i);

        ImU32 fill = IM_COL32(200, 160, 80, 255);
        if (idx == selected_point_) fill = IM_COL32(255, 200, 80, 255);
        else if (idx == hovered_point_) fill = IM_COL32(230, 180, 60, 255);

        draw_list->AddCircleFilled({px, cy}, point_radius, fill);

        ImU32 outline = IM_COL32(180, 180, 180, 160);
        if (idx == selected_point_) outline = IM_COL32(255, 255, 255, 255);
        else if (idx == hovered_point_) outline = IM_COL32(255, 255, 255, 220);
        draw_list->AddCircle({px, cy}, point_radius, outline, 0, 2.0f);

        // Label above
        auto label = fs::path(blend_space_.points()[i].clip).stem().string();
        if (label.empty()) label = "?";
        auto text_size = ImGui::CalcTextSize(label.c_str());
        draw_list->AddText({px - text_size.x * 0.5f, cy - point_radius - text_size.y - 2.0f},
                           IM_COL32(200, 200, 200, 200), label.c_str());
    }

    // Cursor (diamond)
    float cursor_x = to_screen_x(current_param_);
    constexpr float d = 6.0f;
    float cursor_y = cy + 18.0f;
    draw_list->AddQuadFilled(
        {cursor_x, cursor_y - d}, {cursor_x + d, cursor_y},
        {cursor_x, cursor_y + d}, {cursor_x - d, cursor_y},
        IM_COL32(80, 220, 120, 220));
    draw_list->AddQuad(
        {cursor_x, cursor_y - d}, {cursor_x + d, cursor_y},
        {cursor_x, cursor_y + d}, {cursor_x - d, cursor_y},
        IM_COL32(255, 255, 255, 180));

    // Invisible button for interaction
    ImGui::SetCursorScreenPos(canvas_pos);
    ImGui::InvisibleButton("##Canvas1D", {canvas_width, canvas_height});
    bool canvas_active = ImGui::IsItemActive();
    bool canvas_hovered = ImGui::IsItemHovered();

    // Left click: pick up point or move cursor
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hovered_point_ >= 0) {
            dragging_point_ = hovered_point_;
            selected_point_ = hovered_point_;
            dragging_cursor_ = false;
        } else {
            dragging_cursor_ = true;
            dragging_point_ = -1;
            selected_point_ = -1;
            current_param_ = from_screen_x(ImGui::GetMousePos().x);
        }
    }

    if (canvas_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (dragging_point_ >= 0 && dragging_point_ < static_cast<int>(blend_space_.size())) {
            float new_pos = from_screen_x(ImGui::GetMousePos().x);
            auto points = blend_space_.points();
            points[dragging_point_].position = new_pos;
            blend_space_.clear();
            for (auto& pt : points) {
                blend_space_.add(std::move(pt));
            }
            // After re-sorting, find where the dragged point ended up
            for (uint32_t i = 0; i < blend_space_.size(); ++i) {
                if (std::abs(blend_space_.points()[i].position - new_pos) < 0.0001f) {
                    dragging_point_ = static_cast<int>(i);
                    selected_point_ = static_cast<int>(i);
                    break;
                }
            }
        } else if (dragging_cursor_) {
            current_param_ = from_screen_x(ImGui::GetMousePos().x);
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        dragging_point_ = -1;
        dragging_cursor_ = false;
    }

    // Double-click: add new point
    if (canvas_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hovered_point_ < 0) {
        BlendSpacePoint pt;
        pt.position = from_screen_x(ImGui::GetMousePos().x);
        blend_space_.add(std::move(pt));
        selected_point_ = static_cast<int>(blend_space_.size()) - 1;
        reload_clips();
    }

    // Right-click on point: delete
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hovered_point_ >= 0) {
        auto points = blend_space_.points();
        points.erase(points.begin() + hovered_point_);
        blend_space_.clear();
        for (auto& pt : points) {
            blend_space_.add(std::move(pt));
        }
        if (selected_point_ == hovered_point_) selected_point_ = -1;
        else if (selected_point_ > hovered_point_) selected_point_--;
        hovered_point_ = -1;
        reload_clips();
    }

    // Delete key
    if (selected_point_ >= 0 && selected_point_ < static_cast<int>(blend_space_.size()) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        auto points = blend_space_.points();
        points.erase(points.begin() + selected_point_);
        blend_space_.clear();
        for (auto& pt : points) {
            blend_space_.add(std::move(pt));
        }
        selected_point_ = -1;
        reload_clips();
    }

    // Drag-drop onto canvas: onto a point reassigns, onto empty space creates
    if (ImGui::BeginDragDropTarget()) {
        if (auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            auto clip_path = std::string(static_cast<const char*>(payload->Data),
                                         static_cast<size_t>(payload->DataSize - 1));
            if (hovered_point_ >= 0) {
                auto points = blend_space_.points();
                points[hovered_point_].clip = clip_path;
                blend_space_.clear();
                for (auto& pt : points) {
                    blend_space_.add(std::move(pt));
                }
            } else {
                BlendSpacePoint pt;
                pt.clip = clip_path;
                pt.position = from_screen_x(ImGui::GetMousePos().x);
                blend_space_.add(std::move(pt));
                selected_point_ = static_cast<int>(blend_space_.size()) - 1;
            }
            reload_clips();
        }
        ImGui::EndDragDropTarget();
    }

    // Info text
    if (!blend_space_.empty()) {
        auto result = blend_space_.evaluate(current_param_);
        auto na = fs::path(blend_space_.points()[result.index_a].clip).stem().string();
        auto nb = fs::path(blend_space_.points()[result.index_b].clip).stem().string();
        ImGui::TextDisabled("Blend: %s %.0f%% + %s %.0f%%",
                            na.c_str(), result.weight_a * 100.0f,
                            nb.c_str(), result.weight_b * 100.0f);
    } else {
        ImGui::TextDisabled("Drop clips to place, double-click for empty, drag to move");
    }
    ImGui::TextDisabled("Cursor: %.2f", current_param_);
}

void BlendSpace1DEditorContext::draw_point_list() {
    bool changed = false;

    // Parameter name
    char param_buf[128];
    std::strncpy(param_buf, blend_space_.parameter().c_str(), sizeof(param_buf) - 1);
    param_buf[sizeof(param_buf) - 1] = '\0';
    ImGui::Text("Parameter");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##ParamName", param_buf, sizeof(param_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        blend_space_.set_parameter(param_buf);
    }

    ImGui::Spacing();
    ImGui::Separator();

    auto points = blend_space_.points(); // copy
    int remove_idx = -1;

    for (size_t i = 0; i < points.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        bool is_selected = (static_cast<int>(i) == selected_point_);

        // Selectable header
        auto label = fs::path(points[i].clip).stem().string();
        if (label.empty()) label = "(no clip)";
        char header[256];
        std::snprintf(header, sizeof(header), "%.2f  %s", points[i].position, label.c_str());
        if (ImGui::Selectable(header, is_selected)) {
            selected_point_ = static_cast<int>(i);
        }

        // Position
        ImGui::SetNextItemWidth(-30);
        if (ImGui::DragFloat("##Pos", &points[i].position, 0.01f)) {
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            remove_idx = static_cast<int>(i);
        }

        // Clip path with drag-drop
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
        BlendSpacePoint pt;
        pt.position = blend_space_.empty() ? 0.0f : blend_space_.points().back().position + 0.5f;
        points.push_back(std::move(pt));
        selected_point_ = static_cast<int>(points.size()) - 1;
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
    ImGui::TextDisabled("Parameter: %s", blend_space_.parameter().c_str());
}

void BlendSpace1DEditorContext::draw_clip_list() {
    if (available_clip_names_.empty()) {
        ImGui::TextDisabled("Assign a skeleton to see clips");
        return;
    }

    for (int i = 0; i < static_cast<int>(available_clip_names_.size()); ++i) {
        ImGui::PushID(i);

        ImGui::Selectable(available_clip_names_[i].c_str(), false);

        // Drag source so clips can be dragged onto canvas points
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            const auto& path = available_clip_paths_[i];
            ImGui::SetDragDropPayload("ASSET_PATH", path.c_str(), path.size() + 1);
            ImGui::Text("%s", available_clip_names_[i].c_str());
            ImGui::EndDragDropSource();
        }

        ImGui::PopID();
    }
}

void BlendSpace1DEditorContext::load_skeleton(const std::string& skel_path) {
    blend_space_.set_skeleton_path(skel_path);
    scan_available_clips();

    auto resolved = base_path::resolve_asset(skel_path, deps_.project_mgr->active());
    auto model_result = deps_.resource_manager->load_skeletal_model(resolved);
    if (!model_result) {
        FJELL_CORE_ERROR("BS1D: failed to load skeletal model: {}", model_result.error());
        return;
    }

    auto* skel_model = deps_.resource_manager->get(*model_result);
    if (!skel_model || !skel_model->skeleton()) return;

    skeleton_ = skel_model->skeleton_shared();

    preview_scene_.clear();
    auto node = std::make_unique<SceneNode>();
    node->name = "bs1d_preview";

    auto& smc = node->components.add<SkeletalMeshComponent>();
    smc.skeletal_model_handle = *model_result;
    smc.skeletal_model = skel_model;

    auto tex = deps_.resource_manager->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    smc.material_handle = deps_.resource_manager->create_material("bs1d_preview", tex);
    smc.material = deps_.resource_manager->get(smc.material_handle);

    auto& anim = node->components.add<AnimatorComponent>();
    anim.init(skel_model->skeleton());

    preview_node_id_ = preview_scene_.add_node(std::move(node));

    if (auto* cam = viewport_manager_.active_camera()) {
        cam->focus({0.0f, 1.0f, 0.0f}, 2.5f);
    }

    setup_preview_state_machine();
}

void BlendSpace1DEditorContext::setup_preview_state_machine() {
    if (preview_node_id_ == 0 || blend_space_.empty() || !skeleton_) return;

    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* anim = node->components.get<AnimatorComponent>();
    if (!anim) return;

    for (uint32_t i = 0; i < blend_space_.size(); ++i) {
        if (i < loaded_clips_.size() && loaded_clips_[i]) {
            anim->set_clip(blend_space_.points()[i].clip, loaded_clips_[i].get());
        }
    }

    anim->load_blend_space_1d(asset_path_, blend_space_);

    nlohmann::json j;
    j["initial"] = "Preview";
    j["params"] = {{blend_space_.parameter(), 0.0f}};
    j["states"] = nlohmann::json::array();
    j["states"].push_back({
        {"name", "Preview"},
        {"blend_space", asset_path_},
        {"loop", true}
    });

    auto tmp = std::filesystem::temp_directory_path() / "fjell_bs1d_preview.fjanimset";
    {
        std::ofstream f(tmp);
        f << j.dump(2);
    }
    if (auto r = anim->load_state_machine(tmp.string()); !r) {
        FJELL_CORE_WARN("BlendSpace1D: failed to load preview state machine: {}", r.error());
    }
    std::filesystem::remove(tmp);
}

void BlendSpace1DEditorContext::scan_available_clips() {
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

bool BlendSpace1DEditorContext::save() {
    if (asset_path_.empty()) return false;
    auto resolved = base_path::resolve_asset(asset_path_, deps_.project_mgr->active());
    if (auto r = blend_space_.save(resolved); r) {
        FJELL_CORE_INFO("Saved blend space 1D: {}", asset_path_);
        return true;
    } else {
        FJELL_CORE_ERROR("Failed to save blend space 1D: {}", r.error());
        return false;
    }
}

void BlendSpace1DEditorContext::update_preview(float dt) {
    if (preview_node_id_ == 0 || blend_space_.empty()) return;

    auto* node = preview_scene_.find(preview_node_id_);
    if (!node) return;
    auto* anim = node->components.get<AnimatorComponent>();
    if (!anim || !anim->initialized()) return;

    anim->set_param(blend_space_.parameter(), current_param_);

    preview_scene_.compute_world_matrices(nullptr);
    auto& system = deps_.animation_subsystem->system();
    system.update(preview_scene_, dt);
}

// ── Viewport boilerplate ────────────────────────────────────────────────

std::vector<std::string> BlendSpace1DEditorContext::docked_window_names() const {
    return {ctx_title("Parameter"), ctx_title("BS1D Preview"),
            ctx_title("Points"), ctx_title("Clips"), ctx_title("Properties"),
            ctx_title("Stats"), ctx_title("Console"), ctx_title("History")};
}

void BlendSpace1DEditorContext::setup_dockspace(ImGuiID main_area) {
    enum class EPanel { Parameter, Preview, Points, Clips, Properties, Stats, Console, History };
    using N = DockNode;
    static const auto layout = N::horizontal_split(0.22f,
        N::vertical_split(0.40f,
            N::leaf({EPanel::Points}),
            N::vertical_split(0.60f,
                N::leaf({EPanel::Clips}),
                N::leaf({EPanel::Properties}))),
        N::vertical_split(0.75f,
            N::leaf({EPanel::Parameter, EPanel::Preview}),
            N::leaf({EPanel::Stats, EPanel::Console, EPanel::History})));
    build_dockspace(main_area, layout, docked_window_names());
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
