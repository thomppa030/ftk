#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

// Analytic sun shafts: half-res screen-space ray-march against cascade shadow
// map to produce crisp directional-light crepuscular rays. Complements the
// froxel-based volumetric fog (which handles atmospheric haze and point/spot
// volumetrics) with geometrically accurate shafts bounded by shadows.
class SunShaftsPass : public RenderPass {
public:
    static constexpr uint32_t TILE_SIZE = 8;
    static constexpr uint32_t RESOLUTION_DIVISOR = 2;  // half-res
    static constexpr VkFormat SHAFTS_FORMAT = VK_FORMAT_B10G11R11_UFLOAT_PACK32;

    struct Image {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
    };

    // Per-viewport resolution-dependent state.
    struct Images {
        Image shafts;
        Image shafts_history;  // previous-frame reprojection source

        VkExtent2D extent{};
        uint32_t frame_counter{0};

        VkDescriptorPool pool{VK_NULL_HANDLE};
        VkDescriptorSet march_set{VK_NULL_HANDLE};

        void create(VkDevice device, VmaAllocator allocator,
                    VkDescriptorSetLayout march_layout,
                    uint32_t screen_w, uint32_t screen_h);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                    VkDescriptorSetLayout march_layout,
                    uint32_t screen_w, uint32_t screen_h);
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    [[nodiscard]] bool enabled() const { return enabled_; }

    [[nodiscard]] VkDescriptorSetLayout march_set_layout() const { return march_set_layout_; }

private:
    static Image create_image(VkDevice device, VmaAllocator allocator,
                              uint32_t w, uint32_t h);
    static void destroy_image(VkDevice device, VmaAllocator allocator, Image& img);

    // Scene-driven parameters (set by prepare_frame)
    bool enabled_{false};
    float intensity_{1.0f};
    float density_mult_{1.0f};
    float max_reach_{200.0f};
    int step_count_{32};
    float anisotropy_{0.75f};
    float fog_density_{0.0f};  // pulled from VolumetricFogNode

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkDescriptorSetLayout march_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout march_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline march_pipeline_{VK_NULL_HANDLE};

    Images default_images_;
};

} // namespace fjell
