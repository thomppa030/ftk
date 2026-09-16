#pragma once

#include "environment/cloud_shadow_frame.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

// The cloud layer's shadow on the ground: a small top-down map of the sun's
// transmittance through the clouds over a square that follows the camera,
// computed from the same cloud noise the sky shaders draw and projected
// along the shadow-casting body. Bound globally as "cloud_shadow" and
// multiplied into the sun's shadow term by every consumer of the cascades,
// so scattered clouds drag shadow patches across the terrain and an
// overcast sky has no hard shadows at all.
class CloudShadowPass : public RenderPass {
public:
    static constexpr uint32_t RESOLUTION = 256;
    static constexpr float EXTENT_METRES = 512.0f;
    static constexpr VkFormat SHADOW_FORMAT = VK_FORMAT_R8_UNORM;

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    /// Whether the map holds shadows this frame: there is cloud to cast them
    /// and a body above the horizon to cast them from.
    [[nodiscard]] bool active() const { return active_; }
    /// World position → map uv, for the environment UBO.
    [[nodiscard]] const glm::mat4& world_to_uv() const { return frame_.world_to_uv; }

private:
    void create_image();
    void destroy_image();
    void seed_layout();

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};

    VkImage image_{VK_NULL_HANDLE};
    VmaAllocation memory_{VK_NULL_HANDLE};
    VkImageView view_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    bool first_record_{true};

    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet output_set_{VK_NULL_HANDLE};

    VkQueue graphics_queue_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};

    CloudShadowFrame frame_{};
    float ground_y_{0.0f};
    float time_{0.0f};
    bool active_{false};
    bool cleared_{false};   // the image holds all ones from the last inactive frame
};

} // namespace fjell
