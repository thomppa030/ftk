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
    void collect_exports(DeclareContext& ctx) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] VkImageView shadow_view() const { return shadow_view_; }
    [[nodiscard]] VkSampler shadow_sampler() const { return sampler_; }

    /// Grow the shadow image so it covers a viewport of this size. The image
    /// is shared by every viewport and is only ever enlarged, so the largest
    /// viewport seen so far decides its size. Call outside of recording —
    /// this reallocates and rewrites descriptors.
    void ensure_size(uint32_t w, uint32_t h);

private:
    void create_image(uint32_t w, uint32_t h);
    void destroy_image();
    void resize(uint32_t w, uint32_t h);
    void update_descriptor();
    /// Put a freshly created image into SHADER_READ_ONLY_OPTIMAL on the spot.
    /// Consumers sample it through the global descriptor from command buffers
    /// this pass does not record into, so it has to be readable before the
    /// next draw rather than from this pass's own first record().
    void seed_layout();

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

    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet output_set_{VK_NULL_HANDLE};
    uint32_t frame_counter_{0};

    // Kept from post_create so a regrown image can be seeded outside a frame.
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
};

} // namespace fjell
