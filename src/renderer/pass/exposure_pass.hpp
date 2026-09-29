#pragma once

#include "gpu/buffer.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <array>
#include <cstdint>

namespace fjell {

/// Compute pass that builds a luminance histogram from the HDR scene color
/// and reduces it to an average luminance value for auto-exposure.
/// Two dispatches per frame: histogram build (parallel) + reduce (single workgroup).
class ExposurePass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    /// The average luminance the frame that last used `frame_index`'s slot
    /// computed. Complete once the GPU has finished that frame.
    [[nodiscard]] float average_luminance(uint32_t frame_index) const;

    void set_enabled(bool e) { enabled_ = e; }
    [[nodiscard]] bool enabled() const { return enabled_; }

    // Luminance range for histogram binning (log2 space)
    void set_lum_range(float min_log2, float max_log2) {
        min_log_lum_ = min_log2;
        max_log_lum_ = max_log2;
    }

private:
    gpu::Device* device_{nullptr};
    gpu::Owned<gpu::ComputePipeline> pipeline_;
    gpu::Sampler sampler_;

    // Per frame slot: the histogram (256 bins and the average), and the
    // average copied where the CPU reads it once the GPU is done with the slot.
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> histograms_;
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> readbacks_;

    bool enabled_{false};
    float min_log_lum_{-10.0f};
    float max_log_lum_{4.0f};
};

} // namespace fjell
