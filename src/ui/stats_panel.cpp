#include "ui/stats_panel.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/icon_cache.hpp"

#include <imgui.h>

namespace fjell {

void StatsPanel::init(VulkanContext* ctx) {
    ctx_ = ctx;
}

void StatsPanel::draw(float dt, const char* title) {
    // FPS calculation
    frame_count_++;
    fps_timer_ += dt;
    if (fps_timer_ >= 0.5f) {
        fps_ = static_cast<float>(frame_count_) / fps_timer_;
        frame_time_ = fps_timer_ / static_cast<float>(frame_count_) * 1000.0f;
        frame_count_ = 0;
        fps_timer_ = 0.0f;
    }

    frame_time_history_[frame_time_index_] = dt * 1000.0f;
    frame_time_index_ = (frame_time_index_ + 1) % FRAME_TIME_HISTORY_SIZE;

    const auto& stats = ctx_->render_stats();

    if (ImGui::Begin(title)) {
        if (auto* ic = IconCache::instance()) ic->draw_panel_icon("stats");
        ImGui::Text("GPU: %s", ctx_->gpu_name().c_str());
        ImGui::SameLine(0, 20);
#ifdef FJELL_ENABLE_TRACY
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Tracy");
        ImGui::SameLine(0, 20);
#endif
        ImGui::Text("FPS: %.0f", fps_);
        ImGui::SameLine(0, 20);
        ImGui::Text("Frame: %.2f ms", frame_time_);
        ImGui::SameLine(0, 20);
        ImGui::Text("Draw calls: %u", stats.draw_calls);
        ImGui::SameLine(0, 20);
        ImGui::Text("Triangles: %u", stats.triangle_count);
        ImGui::SameLine(0, 20);
        ImGui::Text("Culled: %u/%u", stats.objects_culled, stats.objects_total);
        ImGui::SameLine(0, 20);
        ImGui::Text("MSAA: %ux", static_cast<uint32_t>(ctx_->msaa_samples()));
        if (ctx_->mesh_shaders_available()) {
            ImGui::SameLine(0, 20);
            bool mesh_on = ctx_->use_mesh_shaders();
            if (ImGui::Checkbox("Mesh Shaders", &mesh_on)) {
                ctx_->set_use_mesh_shaders(mesh_on);
            }
            if (mesh_on) {
                ImGui::SameLine(0, 10);
                ImGui::Text("Meshlets: %u", stats.meshlets_dispatched);
                ImGui::SameLine(0, 10);
                bool gpu_cull = ctx_->cull_pass().enabled();
                if (ImGui::Checkbox("GPU Cull", &gpu_cull)) {
                    ctx_->cull_pass().set_enabled(gpu_cull);
                }
                if (stats.two_pass_occlusion) {
                    ImGui::SameLine(0, 10);
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "2-Pass OC");
                }
                if (stats.lod1_draws > 0 || stats.lod2_draws > 0 || stats.distance_culled > 0) {
                    ImGui::SameLine(0, 10);
                    ImGui::Text("LOD: %u/%u/%u D:%u",
                                stats.lod0_draws, stats.lod1_draws, stats.lod2_draws,
                                stats.distance_culled);
                }
            }
        }

        ImGui::PlotLines("##FrameTime", frame_time_history_.data(),
                         FRAME_TIME_HISTORY_SIZE, frame_time_index_,
                         nullptr, 0.0f, 33.3f,
                         {ImGui::GetContentRegionAvail().x, 35});

        // Render pass controls
        if (ImGui::CollapsingHeader("Render Passes")) {
            auto& gtao = ctx_->gtao_pass();
            bool gtao_on = gtao.enabled();
            if (ImGui::Checkbox("GTAO", &gtao_on)) gtao.set_enabled(gtao_on);

            auto& ssr = ctx_->ssr_pass();
            bool ssr_on = ssr.enabled();
            if (ImGui::Checkbox("SSR", &ssr_on)) ssr.set_enabled(ssr_on);
            if (ssr_on) {
                float intensity = ssr.intensity();
                if (ImGui::SliderFloat("Intensity##ssr", &intensity, 0.0f, 2.0f)) ssr.set_intensity(intensity);
                float thickness = ssr.thickness();
                if (ImGui::SliderFloat("Thickness##ssr", &thickness, 0.01f, 2.0f)) ssr.set_thickness(thickness);
            }

            auto& bloom = ctx_->bloom_pass();
            bool bloom_on = bloom.enabled();
            if (ImGui::Checkbox("Bloom", &bloom_on)) bloom.set_enabled(bloom_on);
        }
    }
    ImGui::End();
}

} // namespace fjell
