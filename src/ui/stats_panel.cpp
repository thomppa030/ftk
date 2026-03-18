#include "ui/stats_panel.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/vulkan_context.hpp"

#include <imgui.h>

namespace fjell {

void StatsPanel::init(VulkanContext* ctx) {
    ctx_ = ctx;
}

void StatsPanel::draw(float dt) {
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

    if (ImGui::Begin("Stats")) {
        ImGui::Text("GPU: %s", ctx_->gpu_name().c_str());
        ImGui::SameLine(0, 20);
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
            }
        }

        ImGui::PlotLines("##FrameTime", frame_time_history_.data(),
                         FRAME_TIME_HISTORY_SIZE, frame_time_index_,
                         nullptr, 0.0f, 33.3f,
                         {ImGui::GetContentRegionAvail().x, 35});
    }
    ImGui::End();
}

} // namespace fjell
