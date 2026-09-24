#include "ui/stats_panel.hpp"
#include "renderer/render_pipeline.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/panel_widget.hpp"

#include <imgui.h>

#include <format>
#include <string>

namespace fjell {

namespace {

double bytes_to_mib(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

double reduction_pct(uint32_t logical, uint32_t physical) {
    if (logical == 0) { return 0.0; }
    return 100.0 * (1.0 - (static_cast<double>(physical) / static_cast<double>(logical)));
}

// One measurement: its name in the label column, the value read out beside it.
void metric(const char* label, const std::string& value) {
    ui::row(label, [&] { ui::readout(value.c_str()); });
}

} // namespace

void StatsPanel::init(VulkanContext* ctx) {
    ctx_ = ctx;
}

void StatsPanel::draw(float dt, const char* title) {
    // FPS calculation
    frame_count_++;
    fps_timer_ += dt;
    if (fps_timer_ >= 0.5F) {
        fps_ = static_cast<float>(frame_count_) / fps_timer_;
        frame_time_ = fps_timer_ / static_cast<float>(frame_count_) * 1000.0F;
        frame_count_ = 0;
        fps_timer_ = 0.0F;
    }

    frame_time_history_[frame_time_index_] = dt * 1000.0F;
    frame_time_index_ = (frame_time_index_ + 1) % FRAME_TIME_HISTORY_SIZE;

    if (auto p = Panel(ui::icon::stats, title)) {
        draw_header();
        draw_rendering_section();
        draw_frame_graph_section();
    }
}

void StatsPanel::draw_header() {
    // Always visible, outside any section: the numbers worth a glance.
    if (auto t = ui::PropertyTable("##stats_header")) {
        metric("FPS", std::format("{:.0f}", fps_));
        metric("Frame", std::format("{:.2f} ms", frame_time_));
        metric("GPU", ctx_->gpu_name());
#ifdef FJELL_ENABLE_TRACY
        metric("Profiler", "Tracy");
#endif
    }

    ImGui::PlotLines("##FrameTime", frame_time_history_.data(),
                     FRAME_TIME_HISTORY_SIZE, frame_time_index_,
                     nullptr, 0.0F, 33.3F,
                     {ImGui::GetContentRegionAvail().x, 35});
}

void StatsPanel::draw_rendering_section() {
    if (!ui::section_foldable("Rendering", ui::icon::rendering)) { return; }

    const auto& stats = ctx_->render_stats();
    if (auto t = ui::PropertyTable("##rendering_metrics")) {
        metric("Draw calls", std::to_string(stats.draw_calls));
        metric("Triangles", std::to_string(stats.triangle_count));
        metric("Objects", std::format("{} / {} culled",
                                      stats.objects_total - stats.objects_culled,
                                      stats.objects_culled));
        metric("MSAA", std::format("{}x", static_cast<uint32_t>(ctx_->msaa_samples())));
        metric("Meshlets", std::to_string(stats.meshlets_dispatched));
        if (stats.terrain_patches_dispatched > 0) {
            // Patches handed to the task shader and the triangles they would
            // emit if none were culled. Culling happens on the GPU with no
            // readback, so this is the ceiling, not the drawn count.
            metric("Terrain patches", std::format("{} (max {} tris)",
                                                  stats.terrain_patches_dispatched,
                                                  stats.terrain_triangles_max));
        }
        if (stats.two_pass_occlusion) {
            metric("Occlusion", "2-pass");
        }
        if (stats.lod1_draws > 0 || stats.lod2_draws > 0 || stats.distance_culled > 0) {
            metric("LOD draws", std::format("{} / {} / {} (D: {})",
                                            stats.lod0_draws, stats.lod1_draws,
                                            stats.lod2_draws, stats.distance_culled));
        }
        if (stats.tlas_instances > 0) {
            metric("TLAS", std::format("{} instances, {} off screen",
                                       stats.tlas_instances, stats.gi_only_draws));
        }
    }
}

void StatsPanel::draw_frame_graph_section() {
    if (!ui::section_foldable("Frame graph", ui::icon::frame_graph, false)) { return; }

    auto& pipeline = ctx_->render_pipeline();
    const auto& a = pipeline.aliasing_stats();

    if (auto t = ui::PropertyTable("##fg_live")) {
        ui::row("Aliasing", [&] {
            bool aliasing = pipeline.aliasing_enabled();
            if (ui::checkbox("##aliasing", aliasing).changed) {
                pipeline.set_aliasing_enabled(aliasing);
            }
        });
        metric("Logical -> physical", std::format("{} -> {} ({:.1f}%)",
                                                  a.logical_count, a.physical_count,
                                                  reduction_pct(a.logical_count, a.physical_count)));
        metric("Live images", std::to_string(a.live_images));
        metric("Live VRAM", std::format("{:.2f} MiB", bytes_to_mib(a.live_bytes)));
        metric("Acquires", std::format("{} / frame", a.acquires));
        metric("Allocations", std::format("{} / frame", a.allocations));
    }

    ui::subheading("Peak since startup");
    if (auto t = ui::PropertyTable("##fg_peak")) {
        metric("Logical -> physical", std::format("{} -> {} ({:.1f}%)",
                                                  pipeline.peak_logical(), pipeline.peak_physical(),
                                                  reduction_pct(pipeline.peak_logical(),
                                                                pipeline.peak_physical())));
        metric("Live VRAM", std::format("{:.2f} MiB", bytes_to_mib(pipeline.peak_live_bytes())));
        metric("Saved", std::format("~{:.2f} MiB", bytes_to_mib(pipeline.peak_bytes_saved())));
    }
}

} // namespace fjell
