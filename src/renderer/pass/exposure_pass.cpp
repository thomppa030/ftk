#include "renderer/pass/exposure_pass.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"

#include <cstring>

namespace fjell {

namespace {

struct ExposurePushConstants {
    uint32_t width;
    uint32_t height;
    float min_log_lum;
    float inv_log_range;
    uint32_t pass_id;
};

// 256 bins, then the average they reduce to.
constexpr uint64_t HISTOGRAM_BYTES = 256 * sizeof(uint32_t) + sizeof(float);
constexpr uint64_t AVERAGE_OFFSET = 256 * sizeof(uint32_t);

} // namespace

ftk::Result<> ExposurePass::create(const PassResources& resources) {
    device_ = resources.device;
    sampler_ = device_->sampler({.filter = ftk::gpu::Filter::linear,
                                 .mip_filter = ftk::gpu::Filter::nearest,
                                 .address = ftk::gpu::Address::clamp});
    FTK_TRY(pipeline_, device_->create(ftk::gpu::ComputePipelineDesc{
                             .shader = "shaders/exposure_histogram.comp",
                             .name = "exposure_histogram",
                         }));
    for (uint32_t i = 0; i < ftk::MAX_FRAMES_IN_FLIGHT; ++i) {
        FTK_TRY(histograms_[i], device_->create(ftk::gpu::BufferDesc{
                                      .size = HISTOGRAM_BYTES,
                                      .use = ftk::gpu::BufferUse::storage,
                                      .name = "exposure_histogram",
                                  }));
        FTK_TRY(readbacks_[i], device_->create(ftk::gpu::BufferDesc{
                                     .size = sizeof(float),
                                     .memory = ftk::gpu::Memory::readback,
                                     .name = "exposure_readback",
                                 }));
        // A middle grey until the first frame's average lands.
        const float init = 0.18f;
        std::memcpy(device_->mapped(readbacks_[i]).data(), &init, sizeof(float));
    }
    FTK_GFX_DEBUG("Exposure histogram pass created");
    return {};
}

void ExposurePass::destroy() {
    pipeline_.reset();
    for (auto& buffer : histograms_) buffer.reset();
    for (auto& buffer : readbacks_) buffer.reset();
}

void ExposurePass::record(const FrameContext& ctx) {
    if (!enabled_) return;
    auto* target = ctx.target;
    if (!target) return;
    FTK_PROFILE_SCOPE_N("record_exposure");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Exposure Histogram");

    const auto extent = target->extent();
    const ftk::gpu::Buffer histogram = histograms_[ctx.frame_index];
    const ftk::gpu::Buffer readback = readbacks_[ctx.frame_index];
    const float inv_log_range = 1.0f / (max_log_lum_ - min_log_lum_);

    cmd.set_pipeline(pipeline_);
    cmd.bind({{"hdr_texture", ftk::gpu::sampled(target->color_texture(), sampler_)},
              {"HistogramBuffer", ftk::gpu::storage(histogram)}});

    // Pass 1: count every pixel into the bins.
    cmd.push(ExposurePushConstants{.width = extent.x,
                                   .height = extent.y,
                                   .min_log_lum = min_log_lum_,
                                   .inv_log_range = inv_log_range,
                                   .pass_id = 0});
    cmd.dispatch((extent.x + 15) / 16, (extent.y + 15) / 16, 1);

    // Pass 2: reduce the bins to the average, once every count is in.
    cmd.barrier(histogram, ftk::gpu::Access::storage_buffer_read_write_compute,
                ftk::gpu::Access::storage_buffer_read_write_compute);
    cmd.push(ExposurePushConstants{.width = extent.x,
                                   .height = extent.y,
                                   .min_log_lum = min_log_lum_,
                                   .inv_log_range = inv_log_range,
                                   .pass_id = 1});
    cmd.dispatch(1, 1, 1);

    // The average goes where the CPU reads it, once the GPU has finished
    // this frame.
    cmd.barrier(histogram, ftk::gpu::Access::storage_buffer_read_write_compute, ftk::gpu::Access::copy_src);
    cmd.copy(ftk::gpu::BufferRange(histogram, AVERAGE_OFFSET, sizeof(float)), readback);
    cmd.barrier(readback, ftk::gpu::Access::copy_dst, ftk::gpu::Access::host_read);
}

float ExposurePass::average_luminance(uint32_t frame_index) const {
    if (!readbacks_[frame_index]) return 0.18f;
    float avg = 0.18f;
    std::memcpy(&avg, device_->mapped(readbacks_[frame_index]).data(), sizeof(float));
    return avg;
}

PassSetup ExposurePass::setup() const {
    return {
        .name = "exposure",
        .phase = RenderPhase::PostProcess,
        .sub_order = 25,  // after bloom (0), before the post pass (300)
    };
}

void ExposurePass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    // Off without auto-exposure: nothing declared, and the graph culls it.
    if (!enabled_) { return; }
    auto color = builder.import_named(ctx, "color");
    builder.read(color, ftk::gpu::Access::sampled_compute);

    // The histogram is counted into and reduced in place, then copied from.
    // Every viewport in the frame uses this slot's, so declaring it orders
    // one viewport's count after the last one's copy.
    const uint32_t slot = ctx.frame_index;
    if (histograms_[slot] && readbacks_[slot]) {
        auto histogram = builder.import("exposure_histogram", histograms_[slot]);
        builder.read_write(histogram, ftk::gpu::Access::storage_buffer_read_write_compute);
        builder.read(histogram, ftk::gpu::Access::copy_src);
        auto readback = builder.import("exposure_readback", readbacks_[slot]);
        builder.write(readback, ftk::gpu::Access::copy_dst);
    }
    // The average's only reader is the CPU, on a later frame, which the
    // graph cannot see.
    builder.has_side_effects();
}

} // namespace fjell
