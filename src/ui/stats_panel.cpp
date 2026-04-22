#include "ui/stats_panel.hpp"
#include "renderer/pipeline_cache.hpp"
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
        if (ctx_->mesh_shaders_available() && ctx_->use_mesh_shaders()) {
            ImGui::SameLine(0, 20);
            ImGui::Text("Meshlets: %u", stats.meshlets_dispatched);
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

        ImGui::PlotLines("##FrameTime", frame_time_history_.data(),
                         FRAME_TIME_HISTORY_SIZE, frame_time_index_,
                         nullptr, 0.0f, 33.3f,
                         {ImGui::GetContentRegionAvail().x, 35});

        // Shader compile errors
        if (auto* cache = ctx_->pipeline_cache()) {
            for (uint32_t i = 0; i < cache->shader_count(); ++i) {
                if (cache->has_compile_error(i)) {
                    ImGui::PushStyleColor(ImGuiCol_Text, {0.9f, 0.3f, 0.3f, 1.0f});
                    ImGui::TextWrapped("Shader error [%s]: %s",
                        cache->shader_path(i).c_str(),
                        cache->compile_error(i).c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
    }
    ImGui::End();
}

} // namespace fjell
