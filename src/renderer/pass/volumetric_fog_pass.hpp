#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

#include "renderer/renderer_constants.hpp"

namespace fjell {

class VolumetricFogPass : public RenderPass {
public:
    static constexpr uint32_t TILE_SIZE = 8;
    static constexpr uint32_t DEPTH_SLICES = 64;
    static constexpr uint32_t SCREEN_DIVISOR = 16;
    static constexpr VkFormat VOLUME_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

    struct VolumeImage {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
    };

    // Per-viewport resolution-dependent image + temporal state.
    struct Images {
        VolumeImage scatter_volumes[2];
        VolumeImage integrated_volume;

        VkExtent3D volume_extent{};
        VkDescriptorSet imgui_descriptor{VK_NULL_HANDLE};
        bool volumes_need_init{true};
        uint32_t frame_counter{0};
        uint32_t ping_pong{0};

        // Own pool + all descriptor sets
        VkDescriptorPool pool{VK_NULL_HANDLE};
        VkDescriptorSet inject_sets[2]{};
        VkDescriptorSet temporal_sets[2]{};
        VkDescriptorSet integrate_sets[2]{};
        VkDescriptorSet composite_sets[MAX_FRAMES_IN_FLIGHT]{};

        void create(VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout inject_layout,
                     VkDescriptorSetLayout temporal_layout,
                     VkDescriptorSetLayout integrate_layout,
                     VkDescriptorSetLayout composite_layout,
                     VkSampler linear_sampler,
                     uint32_t vol_w, uint32_t vol_h);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout inject_layout,
                     VkDescriptorSetLayout temporal_layout,
                     VkDescriptorSetLayout integrate_layout,
                     VkDescriptorSetLayout composite_layout,
                     VkSampler linear_sampler,
                     uint32_t screen_w, uint32_t screen_h);

        [[nodiscard]] VkDescriptorSet imgui_texture(VkSampler sampler);
        void invalidate_imgui_texture();
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void build(FrameGraph& graph, ResourceRegistry& registry,
               const FrameContext& ctx) override;

    // Record into externally-provided images.
    void record_into(const FrameContext& ctx, Images& imgs);

    void resize(uint32_t width, uint32_t height);

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();

    // Parameters (public for direct ImGui access)
    bool enabled_{false};
    float density_{0.003f};
    float height_{0.0f};
    float height_falloff_{0.5f};
    float scattering_{0.5f};
    float anisotropy_{0.7f};
    float max_distance_{130.0f};
    float temporal_blend_{0.9f};
    glm::vec3 fog_color_{0.8f, 0.85f, 0.9f};

    // Expose layouts/sampler so external Images can be created
    [[nodiscard]] VkDescriptorSetLayout inject_set_layout() const { return inject_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout temporal_set_layout() const { return temporal_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout integrate_set_layout() const { return integrate_set_layout_; }
    [[nodiscard]] VkDescriptorSetLayout composite_set_layout() const { return composite_set_layout_; }
    [[nodiscard]] VkSampler fog_sampler() const { return linear_sampler_; }

private:
    static VolumeImage create_volume_image(VkDevice device, VmaAllocator allocator,
                                            uint32_t w, uint32_t h, uint32_t d);
    static void destroy_volume_image(VkDevice device, VmaAllocator allocator, VolumeImage& img);

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Resolution-independent (shared across all Images)
    VkSampler linear_sampler_{VK_NULL_HANDLE};

    VkDescriptorSetLayout inject_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout inject_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline inject_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout temporal_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout temporal_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline temporal_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout integrate_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout integrate_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline integrate_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout composite_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout composite_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};

    // Default per-viewport images
    Images default_images_;
};

} // namespace fjell
