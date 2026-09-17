#include "ui/viewport_manager.hpp"
#include "core/profiler.hpp"
#include "renderer/vulkan_context.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>

namespace fjell {

void ViewportManager::init(Device& device, VmaAllocator allocator, VkSampleCountFlagBits msaa_samples) {
    device_ = &device;
    allocator_ = allocator;
    msaa_samples_ = msaa_samples;
}

void ViewportManager::add_viewport(ViewportSource source, const std::string& title) {
    ViewportPanel panel;
    panel.id = next_id_++;
    panel.title = title.empty()
        ? "Viewport " + std::to_string(panel.id)
        : title;
    panel.source = source;

    // Defer OffscreenPass creation to apply_resize() — creating GPU resources
    // during ImGui draw is unsafe (command buffer may be recording).
    panel.needs_offscreen = true;

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

ViewportPanel* ViewportManager::active_panel() {
    for (auto& panel : panels_) {
        if (panel.id == active_id_) return &panel;
    }
    if (!panels_.empty()) return &panels_.front();
    return nullptr;
}

const ViewportPanel* ViewportManager::active_panel() const {
    for (const auto& panel : panels_) {
        if (panel.id == active_id_) return &panel;
    }
    if (!panels_.empty()) return &panels_.front();
    return nullptr;
}

Camera* ViewportManager::active_camera() {
    auto* panel = active_panel();
    return panel ? &panel->camera : nullptr;
}

float ViewportManager::active_aspect() const {
    const auto* panel = active_panel();
    if (panel && panel->size.y > 0) {
        return panel->size.x / panel->size.y;
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
    const auto* panel = active_panel();
    return panel ? panel->pos : glm::vec2{0.0f, 0.0f};
}

void ViewportManager::draw(VulkanContext& ctx, const Input& input, float dt) {
    FJELL_PROFILE_SCOPE_N("viewport_draw");
    bool any_closing = false;
    for (auto it = panels_.begin(); it != panels_.end();) {
        draw_viewport_panel(*it, ctx, input, dt);

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
        // Deferred creation — safe here because begin_frame fence already waited
        if (panel.needs_offscreen && device_) {
            auto w = std::max(static_cast<uint32_t>(panel.size.x), 256u);
            auto h = std::max(static_cast<uint32_t>(panel.size.y), 256u);
            panel.offscreen = std::make_unique<OffscreenPass>(*device_, allocator_, w, h, msaa_samples_);
            panel.needs_offscreen = false;
        }
        if (!panel.offscreen) continue;
        auto vp_w = static_cast<uint32_t>(panel.size.x);
        auto vp_h = static_cast<uint32_t>(panel.size.y);
        if (vp_w > 0 && vp_h > 0) {
            panel.offscreen->resize(vp_w, vp_h);

            // Post-process images — resize via PostProcessResources
            ctx.default_pp().resize(vp_w, vp_h);

            // Contact shadows share one screen-space image across viewports,
            // so it has to cover the largest of them.
            ctx.contact_shadow_pass().ensure_size(vp_w, vp_h);

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
            if (active(ViewportSource::meshlet_viz)) {
                if (auto* mv = ctx.meshlet_viz_pass()) { mv->resize(vp_w, vp_h); }
            }
            if (active(ViewportSource::gpu_cull)) {
                if (auto* cv = ctx.cull_viz_pass()) { cv->resize(vp_w, vp_h); }
            }
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
    auto* panel = active_panel();
    if (panel && panel->offscreen && width > 0 && height > 0) {
        panel->offscreen->resize(width, height);
        panel->size = {static_cast<float>(width), static_cast<float>(height)};
    }
}

VkDescriptorSet ViewportManager::active_texture(VulkanContext& ctx) {
    auto* panel = active_panel();
    if (!panel || !panel->offscreen) return VK_NULL_HANDLE;

    auto& pp = ctx.default_pp();
    switch (panel->source) {
        case ViewportSource::final_color:
            return pp.tonemap() ? pp.tonemap()->imgui_texture(ctx.tonemap_pass().output_sampler()) : VK_NULL_HANDLE;
        case ViewportSource::ao:
            return pp.gtao() ? pp.gtao()->imgui_texture(ctx.gtao_pass().ao_sampler()) : VK_NULL_HANDLE;
        case ViewportSource::depth:        return ctx.depth_viz_pass().imgui_texture();
        case ViewportSource::wireframe:    return ctx.wireframe_pass().imgui_texture();
        case ViewportSource::normals:      return ctx.normal_pass().imgui_texture();
        case ViewportSource::overdraw:     return ctx.overdraw_pass().imgui_texture();
        case ViewportSource::uv:           return ctx.uv_viz_pass().imgui_texture();
        case ViewportSource::light_heatmap:return ctx.light_heatmap_pass().imgui_texture();
        case ViewportSource::light_grid:   return ctx.light_grid_pass().imgui_texture();
        case ViewportSource::meshlet_viz: {
            auto* mv = ctx.meshlet_viz_pass();
            return mv ? mv->imgui_texture() : VK_NULL_HANDLE;
        }
        case ViewportSource::gpu_cull: {
            auto* cv = ctx.cull_viz_pass();
            return cv ? cv->imgui_texture() : VK_NULL_HANDLE;
        }
        case ViewportSource::gi_indirect: {
            auto* si = ctx.default_pp().ss_gi();
            if (si && si->indirect_view != VK_NULL_HANDLE && !si->first_frame)
                return si->indirect_imgui_texture(ctx.ss_gi_pass().linear_sampler());
            auto* di = ctx.default_pp().ddgi();
            return di ? di->indirect_imgui_texture(ctx.ddgi_pass().linear_sampler()) : VK_NULL_HANDLE;
        }
        case ViewportSource::gi_irradiance: {
            auto* di = ctx.default_pp().ddgi();
            return di ? di->irradiance_imgui_texture(ctx.ddgi_pass().linear_sampler()) : VK_NULL_HANDLE;
        }
        case ViewportSource::motion_field:
            return ctx.motion_field_pass().imgui_texture(ctx.ddgi_pass().linear_sampler());
    }
    return panel->offscreen->imgui_texture();
}

uint32_t ViewportManager::active_source_mask() const {
    uint32_t mask = 0;
    for (const auto& panel : panels_) {
        mask |= (1u << static_cast<uint32_t>(panel.source));
    }
    return mask;
}

void ViewportManager::draw_viewport_panel(ViewportPanel& panel, VulkanContext& ctx,
                                           const Input& input, float dt) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.0f, 0.0f, 0.0f, 0.0f});

    if (ImGui::Begin(panel.title.c_str(), &panel.open)) {
        panel.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        panel.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

        if (panel.focused) {
            active_id_ = panel.id;
        }

        // Source selector overlay (top-left corner)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4.0f, 4.0f});
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
        ImGui::PopStyleVar();

        // Viewport area (after source selector)
        auto cursor = ImGui::GetCursorScreenPos();
        auto vp_max = ImGui::GetWindowContentRegionMax();
        auto vp_pos = ImGui::GetWindowPos();
        auto vp_min = ImGui::GetWindowContentRegionMin();
        panel.pos = {cursor.x, cursor.y};
        float w = vp_max.x - vp_min.x;
        float h = vp_pos.y + vp_max.y - cursor.y;
        panel.size = {std::max(w, 0.0f), std::max(h, 0.0f)};

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

        // Store image rect for external use (picking, gizmo)
        panel.img_pos = {panel.pos.x + pad_x, panel.pos.y + pad_y};
        panel.img_size = {img_w, img_h};

        // Display texture
        VkDescriptorSet tex = VK_NULL_HANDLE;
        if (panel.offscreen) {
            auto& pp = ctx.default_pp();
            switch (panel.source) {
                case ViewportSource::final_color:
                    tex = pp.tonemap() ? pp.tonemap()->imgui_texture(ctx.tonemap_pass().output_sampler()) : VK_NULL_HANDLE; break;
                case ViewportSource::ao:
                    tex = pp.gtao() ? pp.gtao()->imgui_texture(ctx.gtao_pass().ao_sampler()) : VK_NULL_HANDLE; break;
                case ViewportSource::depth:        tex = ctx.depth_viz_pass().imgui_texture(); break;
                case ViewportSource::wireframe:    tex = ctx.wireframe_pass().imgui_texture(); break;
                case ViewportSource::normals:      tex = ctx.normal_pass().imgui_texture(); break;
                case ViewportSource::overdraw:     tex = ctx.overdraw_pass().imgui_texture(); break;
                case ViewportSource::uv:           tex = ctx.uv_viz_pass().imgui_texture(); break;
                case ViewportSource::light_heatmap:tex = ctx.light_heatmap_pass().imgui_texture(); break;
                case ViewportSource::light_grid:   tex = ctx.light_grid_pass().imgui_texture(); break;
                case ViewportSource::meshlet_viz: {
                    auto* mv = ctx.meshlet_viz_pass();
                    tex = mv ? mv->imgui_texture() : VK_NULL_HANDLE;
                } break;
                case ViewportSource::gpu_cull: {
                    auto* cv = ctx.cull_viz_pass();
                    tex = cv ? cv->imgui_texture() : VK_NULL_HANDLE;
                } break;
                case ViewportSource::gi_indirect: {
                    auto* si = ctx.default_pp().ss_gi();
                    if (si && si->indirect_view != VK_NULL_HANDLE && !si->first_frame) {
                        tex = si->indirect_imgui_texture(ctx.ss_gi_pass().linear_sampler());
                    } else {
                        auto* di = ctx.default_pp().ddgi();
                        tex = di ? di->indirect_imgui_texture(ctx.ddgi_pass().linear_sampler()) : VK_NULL_HANDLE;
                    }
                } break;
                case ViewportSource::gi_irradiance: {
                    auto* di = ctx.default_pp().ddgi();
                    tex = di ? di->irradiance_imgui_texture(ctx.ddgi_pass().linear_sampler()) : VK_NULL_HANDLE;
                } break;
                case ViewportSource::motion_field:
                    tex = ctx.motion_field_pass().imgui_texture(ctx.ddgi_pass().linear_sampler());
                    break;
            }
        }

        if (tex && img_w > 0 && img_h > 0) {
            if (pad_x > 0 || pad_y > 0) {
                ImGui::SetCursorScreenPos({panel.img_pos.x, panel.img_pos.y});
            }
            ImGui::Image(tex, {img_w, img_h});
        }

        // Camera input — only the active viewport drives the render
        if (panel.hovered && panel.id == active_id_) {
            panel.camera.update(input, dt);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace fjell
