#include "ui/stats_panel.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/render_pipeline.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/icon_cache.hpp"

#include <imgui.h>

namespace fjell {

namespace {

double bytes_to_mib(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

float reduction_pct(uint32_t logical, uint32_t physical) {
    if (logical == 0) { return 0.0F; }
    return 100.0F * (1.0F - (static_cast<float>(physical)
                              / static_cast<float>(logical)));
}

// Two-column metric row: label on the left, value on the right. Stretches
// with the panel width and keeps values lined up regardless of label length.
void metric_row(const char* label, const char* value_fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    va_list args;
    va_start(args, value_fmt);
    ImGui::TextV(value_fmt, args);
    va_end(args);
}

// Begin a two-column key/value table. Caller must ImGui::EndTable() if the
// returned bool is true.
bool begin_metric_table(const char* id) {
    return ImGui::BeginTable(id, 2,
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoBordersInBody);
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

    if (ImGui::Begin(title)) {
        if (auto* ic = IconCache::instance()) { ic->draw_panel_icon("stats"); }

        draw_header();
        draw_rendering_section();
        draw_frame_graph_section();
        draw_shader_errors_section();
    }
    ImGui::End();
}

void StatsPanel::draw_header() {
    // Always-visible at-a-glance: FPS, frame time, frame-time graph. These
    // stay outside any collapsing header so they're useful even at a glance.
    if (begin_metric_table("##stats_header")) {
        metric_row("FPS",        "%.0f",     static_cast<double>(fps_));
        metric_row("Frame",      "%.2f ms",  static_cast<double>(frame_time_));
        metric_row("GPU",        "%s",       ctx_->gpu_name().c_str());
#ifdef FJELL_ENABLE_TRACY
        metric_row("Profiler",   "Tracy");
#endif
        ImGui::EndTable();
    }

    ImGui::PlotLines("##FrameTime", frame_time_history_.data(),
                     FRAME_TIME_HISTORY_SIZE, frame_time_index_,
                     nullptr, 0.0F, 33.3F,
                     {ImGui::GetContentRegionAvail().x, 35});
}

void StatsPanel::draw_rendering_section() {
    if (!ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    const auto& stats = ctx_->render_stats();

    if (begin_metric_table("##rendering_metrics")) {
        metric_row("Draw calls",  "%u", stats.draw_calls);
        metric_row("Triangles",   "%u", stats.triangle_count);
        metric_row("Objects",     "%u / %u culled",
                   stats.objects_total - stats.objects_culled,
                   stats.objects_culled);
        metric_row("MSAA",        "%ux",
                   static_cast<uint32_t>(ctx_->msaa_samples()));

        if (ctx_->mesh_shaders_available() && ctx_->use_mesh_shaders()) {
            metric_row("Meshlets",  "%u", stats.meshlets_dispatched);
            if (stats.two_pass_occlusion) {
                metric_row("Occlusion", "2-Pass");
            }
            if (stats.lod1_draws > 0 || stats.lod2_draws > 0
                || stats.distance_culled > 0) {
                metric_row("LOD draws", "%u / %u / %u (D: %u)",
                           stats.lod0_draws, stats.lod1_draws,
                           stats.lod2_draws, stats.distance_culled);
            }
        }
        ImGui::EndTable();
    }
}

void StatsPanel::draw_frame_graph_section() {
    if (!ImGui::CollapsingHeader("Frame Graph")) { return; }

    auto& pipeline = ctx_->render_pipeline();
    const auto& a = pipeline.aliasing_stats();

    bool aliasing = pipeline.aliasing_enabled();
    if (ImGui::Checkbox("Aliasing enabled", &aliasing)) {
        pipeline.set_aliasing_enabled(aliasing);
    }

    if (begin_metric_table("##fg_live")) {
        metric_row("Logical -> Physical", "%u -> %u (%.1f%%)",
                   a.logical_count, a.physical_count,
                   static_cast<double>(reduction_pct(a.logical_count,
                                                     a.physical_count)));
        metric_row("Live images",   "%u", a.live_images);
        metric_row("Live VRAM",     "%.2f MiB", bytes_to_mib(a.live_bytes));
        metric_row("Acquires",      "%u / frame", a.acquires);
        metric_row("Allocations",   "%u / frame", a.allocations);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Peak since startup");

    if (begin_metric_table("##fg_peak")) {
        metric_row("Logical -> Physical", "%u -> %u (%.1f%%)",
                   pipeline.peak_logical(), pipeline.peak_physical(),
                   static_cast<double>(reduction_pct(pipeline.peak_logical(),
                                                     pipeline.peak_physical())));
        metric_row("Live VRAM",  "%.2f MiB",
                   bytes_to_mib(pipeline.peak_live_bytes()));
        metric_row("Saved",      "~%.2f MiB",
                   bytes_to_mib(pipeline.peak_bytes_saved()));
        ImGui::EndTable();
    }
}

void StatsPanel::draw_shader_errors_section() {
    auto* cache = ctx_->pipeline_cache();
    if (cache == nullptr) { return; }

    bool any_error = false;
    for (uint32_t i = 0; i < cache->shader_count(); ++i) {
        if (cache->has_compile_error(i)) { any_error = true; break; }
    }
    if (!any_error) { return; }

    if (!ImGui::CollapsingHeader("Shader errors",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    for (uint32_t i = 0; i < cache->shader_count(); ++i) {
        if (!cache->has_compile_error(i)) { continue; }
        ImGui::PushStyleColor(ImGuiCol_Text, {0.9F, 0.3F, 0.3F, 1.0F});
        ImGui::TextWrapped("[%s] %s",
            cache->shader_path(i).c_str(),
            cache->compile_error(i).c_str());
        ImGui::PopStyleColor();
    }
}

} // namespace fjell
