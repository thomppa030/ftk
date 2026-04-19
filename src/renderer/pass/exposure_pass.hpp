#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/renderer_constants.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

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

    /// Read back the computed average luminance (from previous frame's buffer).
    [[nodiscard]] float average_luminance(uint32_t frame_index) const;

    void set_enabled(bool e) { enabled_ = e; }
    [[nodiscard]] bool enabled() const { return enabled_; }

    // Luminance range for histogram binning (log2 space)
    void set_lum_range(float min_log2, float max_log2) {
        min_log_lum_ = min_log2;
        max_log_lum_ = max_log2;
    }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sets_{};
    VkSampler sampler_{VK_NULL_HANDLE};

    // Per-frame histogram SSBO (256 uint bins + 1 float avg_luminance) — device-local
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> buffers_;
    // Small host-visible readback buffer (1 float per frame)
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> readback_buffers_;

    bool enabled_{false};
    float min_log_lum_{-10.0f};
    float max_log_lum_{4.0f};
};

} // namespace fjell
