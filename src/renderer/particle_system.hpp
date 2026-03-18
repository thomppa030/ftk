#pragma once

#include "renderer/buffer.hpp"
#include "renderer/particle_emitter.hpp"
#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fjell {

class Device;
class Scene;

class ParticleSystem {
public:
    void create(Device& device, const PassResources& resources);
    void destroy();

    // Sync GPU emitter data with scene emitters (create/destroy as needed)
    void sync_emitters(const Scene& scene);

    // Dispatch compute shaders for all emitters
    void dispatch_compute(VkCommandBuffer cmd, float dt, Scene& scene);

    // Insert SSBO barriers between compute and render
    void insert_barriers(VkCommandBuffer cmd);

    // Record particle billboard rendering
    void record_render(const FrameContext& ctx);

private:
    struct EmitterGpu {
        uint32_t object_id{0};
        uint32_t max_particles{0};

        std::unique_ptr<Buffer> particle_buffer;
        std::unique_ptr<Buffer> counter_buffer;
        std::unique_ptr<Buffer> dead_list_buffer;
        std::unique_ptr<Buffer> indirect_buffer;

        VkDescriptorSet compute_set{VK_NULL_HANDLE};
        VkDescriptorSet indirect_set{VK_NULL_HANDLE};
        VkDescriptorSet render_set{VK_NULL_HANDLE};

        bool initialized{false};
    };

    void create_emitter_gpu(EmitterGpu& gpu, uint32_t max_particles);
    void destroy_emitter_gpu(EmitterGpu& gpu);
    void init_dead_list(EmitterGpu& gpu);

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};

    // Compute pipelines (shared)
    VkDescriptorSetLayout compute_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout compute_layout_{VK_NULL_HANDLE};
    VkPipeline emit_pipeline_{VK_NULL_HANDLE};
    VkPipeline sim_pipeline_{VK_NULL_HANDLE};

    // Indirect prepare pipeline
    VkDescriptorSetLayout indirect_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout indirect_layout_{VK_NULL_HANDLE};
    VkPipeline indirect_pipeline_{VK_NULL_HANDLE};

    // Render pipelines (shared)
    VkDescriptorSetLayout render_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout render_layout_{VK_NULL_HANDLE};
    VkPipeline render_additive_{VK_NULL_HANDLE};
    VkPipeline render_alpha_{VK_NULL_HANDLE};

    // Descriptor pool for all emitters
    VkDescriptorPool pool_{VK_NULL_HANDLE};

    // Per-emitter GPU data
    std::vector<EmitterGpu> emitters_;

    // Cached layouts
    VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
    VkFormat color_format_{VK_FORMAT_UNDEFINED};
    VkFormat depth_format_{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits sample_count_{VK_SAMPLE_COUNT_1_BIT};

    uint32_t frame_seed_{0};
};

} // namespace fjell
