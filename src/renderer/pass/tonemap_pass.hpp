#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace fjell {

// AgX tone mapping pass. Reads HDR input (from bloom composite or
// raw scene color), outputs LDR sRGB.
class TonemapPass : public RenderPass {
public:
    // Per-viewport resolution-dependent image state.
    struct Images {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkExtent2D extent{};
        VkDescriptorSet imgui_descriptor{VK_NULL_HANDLE};

        // Own pool + per-frame input sets (self-contained allocation)
        VkDescriptorPool pool{VK_NULL_HANDLE};
        VkDescriptorSet input_sets[MAX_FRAMES_IN_FLIGHT]{};

        void create(VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout set_layout,
                     uint32_t width, uint32_t height);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout set_layout,
                     uint32_t width, uint32_t height);

        [[nodiscard]] VkDescriptorSet imgui_texture(VkSampler sampler);
        void invalidate_imgui_texture();
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void build(FrameGraph& graph, ResourceRegistry& registry,
               const FrameContext& ctx) override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    [[nodiscard]] bool uses_declare() const noexcept override { return true; }
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    // Record into externally-provided images.
    void record_into(const FrameContext& ctx, Images& imgs, VkImageView source,
                     VkImageView bloom_view = VK_NULL_HANDLE, float bloom_intensity = 0.3f,
                     float exposure = 0.0f);

    void set_exposure(float ev) { exposure_ = ev; }
    [[nodiscard]] float exposure() const { return exposure_; }

    void set_auto_exposure(bool on) { auto_exposure_ = on; }
    [[nodiscard]] bool auto_exposure() const { return auto_exposure_; }

    void set_adaptation_speed(float s) { adaptation_speed_ = s; }
    [[nodiscard]] float adaptation_speed() const { return adaptation_speed_; }

    void set_exposure_range(float min_ev, float max_ev) { min_ev_ = min_ev; max_ev_ = max_ev; }

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();
    void resize(uint32_t width, uint32_t height);

    // Set the HDR source to read from (bloom output or raw scene color)
    void set_source_view(VkImageView view) { source_view_ = view; }

    // Output image accessors (for overlays rendered after tonemap)
    [[nodiscard]] VkImage output_image() const { return default_images_.image; }
    [[nodiscard]] VkImageView output_view() const { return default_images_.view; }
    [[nodiscard]] VkExtent2D output_extent() const { return default_images_.extent; }

    // Expose set layout and sampler so external Images can be created/displayed
    [[nodiscard]] VkDescriptorSetLayout image_set_layout() const { return set_layout_; }
    [[nodiscard]] VkSampler output_sampler() const { return sampler_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Resolution-independent (shared across all Images)
    VkSampler sampler_{VK_NULL_HANDLE};
    VkSampler input_sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    VkImageView source_view_{VK_NULL_HANDLE};
    float exposure_{0.0f};
    bool auto_exposure_{false};
    float adaptation_speed_{1.5f};
    float min_ev_{-5.0f};
    float max_ev_{5.0f};

    // Default per-viewport images (backward compat — used by record()/resize())
    Images default_images_;
};

} // namespace fjell
