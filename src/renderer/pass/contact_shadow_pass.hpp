#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

// Screen-space ray-march against the depth buffer along the directional
// light direction to recover contact shadows where cascade shadow-map
// resolution is too coarse. Produces an R8 shadow mask at full res that
// the main lighting pass multiplies into its directional shadow term.
//
// References:
//   - Bungie, "Contact hardening shadows in Destiny" (GDC 2015)
//   - Wronski, "Screen Space Ray Traced Shadows" (SIGGRAPH 2014)
class ContactShadowPass : public RenderPass {
public:
    static constexpr uint32_t TILE_SIZE = 8;
    static constexpr VkFormat SHADOW_FORMAT = VK_FORMAT_R8_UNORM;

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] VkImageView shadow_view() const { return shadow_view_; }
    [[nodiscard]] VkSampler shadow_sampler() const { return sampler_; }

private:
    void create_image(uint32_t w, uint32_t h);
    void destroy_image();
    void resize(uint32_t w, uint32_t h);
    void update_descriptor();

    bool enabled_{true};
    float max_distance_{0.3f};
    float thickness_{0.05f};
    int step_count_{12};

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};

    VkImage shadow_image_{VK_NULL_HANDLE};
    VmaAllocation shadow_memory_{VK_NULL_HANDLE};
    VkImageView shadow_view_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    bool first_record_{true};

    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet output_set_{VK_NULL_HANDLE};
    uint32_t frame_counter_{0};
};

} // namespace fjell
