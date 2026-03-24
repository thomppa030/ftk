#include "ui/viewport_manager.hpp"
#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "core/profiler.hpp"
#include "renderer/mesh_component.hpp"
#include "renderer/model.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/scene.hpp"
#include "scene/transform.hpp"
#include "ui/icon_cache.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <limits>

namespace fjell {

void ViewportManager::init(Device& device, VkSampleCountFlagBits msaa_samples) {
    device_ = &device;
    msaa_samples_ = msaa_samples;
}

void ViewportManager::add_viewport(ViewportSource source, const std::string& title) {
    ViewportPanel panel;
    panel.id = next_id_++;
    panel.title = title.empty()
        ? "Viewport " + std::to_string(panel.id)
        : title;
    panel.source = source;

    // Create per-panel offscreen pass with a default size
    if (device_) {
        panel.offscreen = std::make_unique<OffscreenPass>(*device_, 256, 256, msaa_samples_);
    }

    // Dock new viewports as tabs next to the first existing viewport
    if (!panels_.empty()) {
        ImGuiID dock_id = ImGui::FindWindowByName(panels_.front().title.c_str())
            ? ImGui::FindWindowByName(panels_.front().title.c_str())->DockId
            : 0;
        if (dock_id != 0) {
            ImGui::DockBuilderDockWindow(panel.title.c_str(), dock_id);
        }
    }

    panels_.push_back(std::move(panel));

    if (active_id_ == 0) {
        active_id_ = panels_.back().id;
    }
}

Camera* ViewportManager::active_camera() {
    for (auto& panel : panels_) {
        if (panel.id == active_id_) return &panel.camera;
    }
    if (!panels_.empty()) return &panels_.front().camera;
    return nullptr;
}

float ViewportManager::active_aspect() const {
    for (const auto& panel : panels_) {
        if (panel.id == active_id_ && panel.size.y > 0) {
            return panel.size.x / panel.size.y;
        }
    }
    return 16.0f / 9.0f;
}

bool ViewportManager::any_hovered() const {
    for (const auto& panel : panels_) {
        if (panel.hovered) return true;
    }
    return false;
}

glm::vec2 ViewportManager::active_viewport_pos() const {
    for (const auto& panel : panels_) {
        if (panel.id == active_id_) return panel.pos;
    }
    return {0.0f, 0.0f};
}

void ViewportManager::draw(VulkanContext& ctx, Scene& scene,
                           uint32_t& selected_id, const Input& input, float dt,
                           int gizmo_op, CommandHistory* history) {
    FJELL_PROFILE_SCOPE_N("viewport_draw");
    bool any_closing = false;
    for (auto it = panels_.begin(); it != panels_.end();) {
        draw_viewport_panel(*it, ctx, scene, selected_id, input, dt, gizmo_op, history);

        if (!it->open) {
            any_closing = true;
        }
        ++it;
    }

    // Wait for GPU before destroying offscreen resources
    if (any_closing) {
        ctx.wait_idle();
    }

    for (auto it = panels_.begin(); it != panels_.end();) {
        if (!it->open) {
            if (it->id == active_id_) active_id_ = 0;
            it = panels_.erase(it);
        } else {
            ++it;
        }
    }

    // If active was removed, pick first available
    if (active_id_ == 0 && !panels_.empty()) {
        active_id_ = panels_.front().id;
    }
}

void ViewportManager::apply_resize(VulkanContext& ctx) {
    FJELL_PROFILE_SCOPE_N("viewport_apply_resize");
    for (auto& panel : panels_) {
        if (!panel.offscreen) continue;
        auto vp_w = static_cast<uint32_t>(panel.size.x);
        auto vp_h = static_cast<uint32_t>(panel.size.y);
        if (vp_w > 0 && vp_h > 0) {
            panel.offscreen->resize(vp_w, vp_h);

            // Core pipeline passes — always resize
            ctx.bloom_pass().resize(vp_w, vp_h);
            ctx.gtao_pass().resize(vp_w, vp_h);
            ctx.ssr_pass().resize(vp_w, vp_h);
            ctx.volumetric_fog_pass().resize(vp_w, vp_h);
            ctx.tonemap_pass().resize(vp_w, vp_h);

            // Viz passes — only resize when active
            uint32_t mask = ctx.active_sources();
            auto active = [mask](ViewportSource src) {
                return mask & (1u << static_cast<uint32_t>(src));
            };
            if (active(ViewportSource::depth))         ctx.depth_viz_pass().resize(vp_w, vp_h);
            if (active(ViewportSource::wireframe))     ctx.wireframe_pass().resize(vp_w, vp_h);
            if (active(ViewportSource::normals))       ctx.normal_pass().resize(vp_w, vp_h);
            if (active(ViewportSource::overdraw))      ctx.overdraw_pass().resize(vp_w, vp_h);
            if (active(ViewportSource::uv))            ctx.uv_viz_pass().resize(vp_w, vp_h);
            if (active(ViewportSource::light_heatmap)) ctx.light_heatmap_pass().resize(vp_w, vp_h);
            if (active(ViewportSource::light_grid))    ctx.light_grid_pass().resize(vp_w, vp_h);
            if (active(ViewportSource::meshlet_viz))   ctx.meshlet_viz_pass().resize(vp_w, vp_h);
            if (active(ViewportSource::gpu_cull))      ctx.cull_viz_pass().resize(vp_w, vp_h);
        }
    }
}

std::vector<ViewportRenderRequest> ViewportManager::build_render_requests() {
    FJELL_PROFILE_SCOPE_N("build_render_requests");
    std::vector<ViewportRenderRequest> requests;
    requests.reserve(panels_.size());
    for (auto& panel : panels_) {
        if (!panel.offscreen) continue;
        if (panel.size.x <= 0 || panel.size.y <= 0) continue;

        float aspect = panel.size.x / std::max(panel.size.y, 1.0f);
        ViewportRenderRequest req;
        req.target = panel.offscreen.get();
        req.view = panel.camera.view_matrix();
        req.projection = panel.camera.projection_matrix(aspect);
        req.camera_position = panel.camera.position();
        requests.push_back(req);
    }
    return requests;
}

void ViewportManager::release_textures() {
    for (auto& panel : panels_) {
        if (panel.offscreen) {
            panel.offscreen->invalidate_imgui_texture();
            panel.offscreen->invalidate_depth_imgui_texture();
        }
    }
}

void ViewportManager::resize_active(uint32_t width, uint32_t height) {
    for (auto& panel : panels_) {
        if (panel.id == active_id_ && panel.offscreen) {
            if (width > 0 && height > 0) {
                panel.offscreen->resize(width, height);
                panel.size = {static_cast<float>(width), static_cast<float>(height)};
            }
            break;
        }
    }
}

VkDescriptorSet ViewportManager::active_texture(VulkanContext& ctx) {
    for (auto& panel : panels_) {
        if (panel.id != active_id_ || !panel.offscreen) continue;

        switch (panel.source) {
            case ViewportSource::final_color:
                return ctx.tonemap_pass().imgui_texture();
            case ViewportSource::depth:
                return ctx.depth_viz_pass().imgui_texture();
            case ViewportSource::wireframe:
                return ctx.wireframe_pass().imgui_texture();
            case ViewportSource::bloom:
                return ctx.bloom_pass().imgui_texture();
            case ViewportSource::normals:
                return ctx.normal_pass().imgui_texture();
            case ViewportSource::overdraw:
                return ctx.overdraw_pass().imgui_texture();
            case ViewportSource::uv:
                return ctx.uv_viz_pass().imgui_texture();
            case ViewportSource::light_heatmap:
                return ctx.light_heatmap_pass().imgui_texture();
            case ViewportSource::light_grid:
                return ctx.light_grid_pass().imgui_texture();
            case ViewportSource::meshlet_viz:
                return ctx.meshlet_viz_pass().imgui_texture();
            case ViewportSource::ao:
                return ctx.gtao_pass().imgui_texture();
            case ViewportSource::gpu_cull:
                return ctx.cull_viz_pass().imgui_texture();
        }
        return panel.offscreen->imgui_texture();
    }
    return VK_NULL_HANDLE;
}

uint32_t ViewportManager::active_source_mask() const {
    uint32_t mask = 0;
    for (const auto& panel : panels_) {
        mask |= (1u << static_cast<uint32_t>(panel.source));
    }
    return mask;
}

void ViewportManager::draw_viewport_panel(ViewportPanel& panel, VulkanContext& ctx,
                                           Scene& scene, uint32_t& selected_id,
                                           const Input& input, float dt, int gizmo_op,
                                           CommandHistory* history) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.0f, 0.0f, 0.0f, 0.0f});

    if (ImGui::Begin(panel.title.c_str(), &panel.open)) {
        panel.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        panel.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

        if (panel.focused) {
            active_id_ = panel.id;
        }

        // Toolbar
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4.0f, 4.0f});

        // Source selector (left)
        const char* current_name = viewport_source_name(panel.source);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::BeginCombo("##Source", current_name)) {
            for (int i = 0; i < VIEWPORT_SOURCE_COUNT; ++i) {
                auto src = static_cast<ViewportSource>(i);
                bool selected = (panel.source == src);
                if (ImGui::Selectable(viewport_source_name(src), selected)) {
                    panel.source = src;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        bool show_col = ctx.show_colliders();
        if (ImGui::Checkbox("Colliders", &show_col)) {
            ctx.set_show_colliders(show_col);
        }

        // Play/Pause/Stop (centered)
        draw_play_toolbar();

        ImGui::PopStyleVar();

        // Viewport area (after toolbar)
        auto cursor = ImGui::GetCursorScreenPos();
        auto vp_max = ImGui::GetWindowContentRegionMax();
        auto vp_pos = ImGui::GetWindowPos();
        auto vp_min = ImGui::GetWindowContentRegionMin();
        panel.pos = {cursor.x, cursor.y};
        float w = vp_max.x - vp_min.x;
        float h = vp_pos.y + vp_max.y - cursor.y;
        panel.size = {std::max(w, 0.0f), std::max(h, 0.0f)};

        // Size is applied later via apply_resize(), not during draw

        // Compute the rendered image region within the panel (letterboxed)
        float img_w = panel.size.x;
        float img_h = panel.size.y;
        float pad_x = 0.0f;
        float pad_y = 0.0f;
        if (panel.offscreen && panel.size.x > 0 && panel.size.y > 0) {
            auto extent = panel.offscreen->extent();
            float tex_aspect = static_cast<float>(extent.width) /
                               std::max(static_cast<float>(extent.height), 1.0f);
            float panel_aspect = panel.size.x / std::max(panel.size.y, 1.0f);
            if (tex_aspect > panel_aspect) {
                img_h = img_w / tex_aspect;
            } else {
                img_w = img_h * tex_aspect;
            }
            pad_x = (panel.size.x - img_w) * 0.5f;
            pad_y = (panel.size.y - img_h) * 0.5f;
        }

        // Display texture from this panel's own offscreen pass
        VkDescriptorSet tex = VK_NULL_HANDLE;
        if (panel.offscreen) {
            switch (panel.source) {
                case ViewportSource::final_color:
                    tex = ctx.tonemap_pass().imgui_texture();
                    break;
                case ViewportSource::depth:
                    tex = ctx.depth_viz_pass().imgui_texture();
                    break;
                case ViewportSource::wireframe:
                    tex = ctx.wireframe_pass().imgui_texture();
                    break;
                case ViewportSource::bloom:
                    tex = ctx.bloom_pass().imgui_texture();
                    break;
                case ViewportSource::normals:
                    tex = ctx.normal_pass().imgui_texture();
                    break;
                case ViewportSource::overdraw:
                    tex = ctx.overdraw_pass().imgui_texture();
                    break;
                case ViewportSource::uv:
                    tex = ctx.uv_viz_pass().imgui_texture();
                    break;
                case ViewportSource::light_heatmap:
                    tex = ctx.light_heatmap_pass().imgui_texture();
                    break;
                case ViewportSource::light_grid:
                    tex = ctx.light_grid_pass().imgui_texture();
                    break;
                case ViewportSource::meshlet_viz:
                    tex = ctx.meshlet_viz_pass().imgui_texture();
                    break;
                case ViewportSource::ao:
                    tex = ctx.gtao_pass().imgui_texture();
                    break;
                case ViewportSource::gpu_cull:
                    tex = ctx.cull_viz_pass().imgui_texture();
                    break;
            }
        }

        if (tex && img_w > 0 && img_h > 0) {
            if (pad_x > 0 || pad_y > 0) {
                ImGui::SetCursorScreenPos({panel.pos.x + pad_x, panel.pos.y + pad_y});
            }
            ImGui::Image(tex, {img_w, img_h});
        }

        // Camera input — only the active viewport drives the render
        if (panel.hovered && panel.id == active_id_) {
            panel.camera.update(input, dt);
        }

        // Mouse picking — skip when interacting with a gizmo
        if (panel.hovered && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver() &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
             ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
            auto mouse = ImGui::GetMousePos();
            float mx = (mouse.x - panel.pos.x - pad_x) / img_w;
            float my = (mouse.y - panel.pos.y - pad_y) / img_h;

            float aspect = img_w / std::max(img_h, 1.0f);
            auto view = panel.camera.view_matrix();
            auto proj = panel.camera.projection_matrix(aspect);
            auto inv_vp = glm::inverse(proj * view);

            glm::vec4 ndc_near{mx * 2.0f - 1.0f, my * 2.0f - 1.0f, 0.0f, 1.0f};
            glm::vec4 ndc_far {mx * 2.0f - 1.0f, my * 2.0f - 1.0f, 1.0f, 1.0f};

            glm::vec4 world_near = inv_vp * ndc_near;
            glm::vec4 world_far  = inv_vp * ndc_far;
            world_near /= world_near.w;
            world_far  /= world_far.w;

            glm::vec3 ray_origin = glm::vec3(world_near);
            glm::vec3 ray_dir = glm::normalize(glm::vec3(world_far - world_near));

            float closest_t = std::numeric_limits<float>::max();
            uint32_t hit_id = 0;

            for (const auto& obj : scene.objects()) {
                auto* mesh = obj->components.get<MeshComponent>();
                if (!mesh || !mesh->model) continue;
                if (!mesh->model->local_aabb().valid()) continue;
                const auto* spatial = obj->as_spatial();
                if (!spatial) continue;

                AABB world_aabb = mesh->model->local_aabb().transform(spatial->world_matrix_);
                glm::vec3 aabb_min = world_aabb.min;
                glm::vec3 aabb_max = world_aabb.max;

                glm::vec3 inv_dir = 1.0f / ray_dir;
                glm::vec3 t1 = (aabb_min - ray_origin) * inv_dir;
                glm::vec3 t2 = (aabb_max - ray_origin) * inv_dir;
                glm::vec3 t_min_v = glm::min(t1, t2);
                glm::vec3 t_max_v = glm::max(t1, t2);
                float t_enter = std::max({t_min_v.x, t_min_v.y, t_min_v.z});
                float t_exit  = std::min({t_max_v.x, t_max_v.y, t_max_v.z});

                if (t_enter <= t_exit && t_exit >= 0.0f) {
                    float t = t_enter >= 0.0f ? t_enter : t_exit;
                    if (t < closest_t) {
                        closest_t = t;
                        hit_id = obj->id;
                    }
                }
            }

            if (hit_id != 0) {
                selected_id = hit_id;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (auto* hit_obj = scene.find_spatial(hit_id)) {
                        glm::vec3 center = glm::vec3(hit_obj->world_matrix_[3]);
                        float radius = 1.0f;
                        auto* hit_mesh = hit_obj->components.get<MeshComponent>();
                        if (hit_mesh && hit_mesh->model &&
                            !hit_mesh->model->collision_vertices().empty()) {
                            float max_r2 = 0.0f;
                            for (const auto& v : hit_mesh->model->collision_vertices()) {
                                auto wv = glm::vec3(hit_obj->world_matrix_ * glm::vec4(v, 1.0f));
                                max_r2 = std::max(max_r2, glm::dot(wv - center, wv - center));
                            }
                            radius = std::max(std::sqrt(max_r2), 0.1f);
                        }
                        panel.camera.focus(center, radius);
                    }
                }
            } else {
                selected_id = 0;
            }
        }

        // Gizmo — ImGuizmo expects OpenGL-style projection (Y-up NDC),
        // so undo the Vulkan Y-flip for the gizmo only.
        if (auto* obj = scene.find_spatial(selected_id); obj) {
            float aspect = img_w / std::max(img_h, 1.0f);
            auto view = panel.camera.view_matrix();
            auto projection = panel.camera.projection_matrix(aspect);
            projection[1][1] *= -1.0f; // undo Vulkan Y-flip for ImGuizmo

            ImGuizmo::SetRect(panel.pos.x + pad_x, panel.pos.y + pad_y,
                              img_w, img_h);
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist();

            // Snapshot at drag start so we can apply delta to the original transform
            if (!panel.gizmo_dragging) {
                panel.gizmo_matrix = obj->world_matrix_;
                panel.gizmo_start_world = obj->world_matrix_;
                panel.gizmo_start_transform = obj->transform;
            }

            ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
            if (gizmo_op == 1) op = ImGuizmo::ROTATE;
            if (gizmo_op == 2) op = ImGuizmo::SCALE;

            ImGuizmo::Manipulate(
                glm::value_ptr(view),
                glm::value_ptr(projection),
                op,
                ImGuizmo::LOCAL,
                glm::value_ptr(panel.gizmo_matrix));

            auto apply_gizmo = [&] {
                // Quaternion-based decompose roundtrips perfectly — no Euler issues
                glm::mat4 parent_world{1.0f};
                if (obj->parent_id != -1) {
                    const auto* parent = scene.find(
                        static_cast<uint32_t>(obj->parent_id));
                    const auto* parent_spatial = parent ? parent->as_spatial() : nullptr;
                    if (parent_spatial) parent_world = parent_spatial->world_matrix_;
                }
                glm::mat4 local_matrix = glm::inverse(parent_world) * panel.gizmo_matrix;
                obj->transform = decompose_matrix(local_matrix);
                obj->world_matrix_ = panel.gizmo_matrix;
            };

            if (ImGuizmo::IsUsing()) {
                panel.gizmo_dragging = true;
                apply_gizmo();
                scene.mark_dirty();
            } else if (panel.gizmo_dragging) {
                panel.gizmo_dragging = false;
                apply_gizmo();
                if (history) {
                    history->execute(std::make_unique<TransformCommand>(
                        scene, obj->id,
                        panel.gizmo_start_transform, obj->transform,
                        obj->name));
                }
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void ViewportManager::set_playing(bool p) { playing_ = p; }

void ViewportManager::draw_play_toolbar() {
    auto* icons = IconCache::instance();
    float icon_size = ImGui::GetFrameHeight() - 4.0f;

    auto icon_button = [&](const char* id, const char* icon_name,
                           const ImVec4& tint = {1, 1, 1, 1}) -> bool {
        auto desc = icons ? icons->icon(icon_name) : VK_NULL_HANDLE;
        ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.1f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.2f});
        bool clicked = false;
        if (desc) {
            clicked = ImGui::ImageButton(id, (ImTextureID)desc, {icon_size, icon_size},
                                         {0, 0}, {1, 1}, {0, 0, 0, 0}, tint);
        } else {
            clicked = ImGui::SmallButton(id);
        }
        ImGui::PopStyleColor(3);
        return clicked;
    };

    // Center the button group
    float button_w = icon_size + ImGui::GetStyle().FramePadding.x * 2.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float group_w = button_w * 3.0f + spacing * 2.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine((avail - group_w) * 0.5f + ImGui::GetCursorPosX());

    if (playing_) {
        // Pause (stub)
        if (icon_button("##pause", "pause", {1.0f, 0.85f, 0.3f, 1.0f})) {
            on_pause.broadcast();
        }
        ImGui::SameLine();
        // Stop
        if (icon_button("##stop", "stop", {1.0f, 0.4f, 0.4f, 1.0f})) {
            on_stop.broadcast();
        }
        ImGui::SameLine();
        // Step (stub)
        if (icon_button("##step", "step", {0.7f, 0.85f, 1.0f, 1.0f})) {
            // stub — single-frame step, not wired yet
        }
    } else {
        // Play
        if (icon_button("##play", "play", {0.4f, 1.0f, 0.5f, 1.0f})) {
            on_play.broadcast();
        }
    }
}

} // namespace fjell
