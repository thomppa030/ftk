#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <array>
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

    struct Images {
        // Ping-pong: each frame writes to shafts[cur] and samples shafts[1-cur]
        // as history for temporal reprojection.
        std::array<Image, 2> shafts{};
        std::array<bool, 2> shafts_ready{};  // layout is SHADER_READ_ONLY after first write

        VkExtent2D extent{};
        uint32_t ping_pong{0};
        uint32_t frame_counter{0};

        VkDescriptorPool pool{VK_NULL_HANDLE};
        // Per-frame-in-flight descriptor sets so we never update a set while
        // a previous frame's command buffer is still using it.
        std::array<std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>, 2> march_sets{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> composite_sets{};

        void create(VkDevice device, VmaAllocator allocator,
                    VkDescriptorSetLayout march_layout,
                    VkDescriptorSetLayout composite_layout,
                    uint32_t screen_w, uint32_t screen_h);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                    VkDescriptorSetLayout march_layout,
                    VkDescriptorSetLayout composite_layout,
                    uint32_t screen_w, uint32_t screen_h);
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void build(FrameGraph& graph, ResourceRegistry& registry,
               const FrameContext& ctx) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    [[nodiscard]] bool enabled() const { return enabled_; }

private:
    static Image create_image(VkDevice device, VmaAllocator allocator,
                              uint32_t w, uint32_t h);
    static void destroy_image(VkDevice device, VmaAllocator allocator, Image& img);

    bool enabled_{false};
    float intensity_{1.0f};
    float density_mult_{1.0f};
    float max_reach_{200.0f};
    int step_count_{32};
    float anisotropy_{0.75f};
    float halo_inner_{0.95f};
    float halo_outer_{0.6f};
    float fog_density_{0.0f};

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkSampler linear_sampler_{VK_NULL_HANDLE};

    VkDescriptorSetLayout march_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout march_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline march_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout composite_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout composite_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};

    Images default_images_;
};

} // namespace fjell
