#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "renderer/renderer_constants.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

namespace fjell {

// GPU-side emitter metadata (128 bytes, matches GLSL EmitterData)
struct GpuEmitterData {
    glm::vec3 emitter_position; float dt;
    glm::vec3 gravity;          uint32_t max_particles;
    glm::vec4 color_start;
    glm::vec4 color_end;
    float size_start, size_end, lifetime_min, lifetime_max;
    glm::vec3 velocity_min;     uint32_t emit_count;
    glm::vec3 velocity_max;     uint32_t random_seed;
    uint32_t base_offset;
    uint32_t counter_offset;
    uint32_t dead_list_offset;
    uint32_t module_offset;       // offset into module buffer (in uint32_t units)
};
static_assert(sizeof(GpuEmitterData) == 128);

// Push constants for all compute shaders (16 bytes)
struct ParticleComputePush {
    float dt;
    uint32_t random_seed;
    uint32_t emitter_count;
    uint32_t total_max_particles;
};
static_assert(sizeof(ParticleComputePush) == 16);

class Device;
class Scene;

class ParticleSystem {
public:
    void create(Device& device, const PassResources& resources);
    void destroy();
    void recreate_pipelines();

    void sync_emitters(Scene& scene);
    void dispatch_compute(VkCommandBuffer cmd, float dt, Scene& scene,
                          VkDescriptorSet global_set = VK_NULL_HANDLE);
    void insert_barriers(VkCommandBuffer cmd);
    void record_render(const FrameContext& ctx);

private:
    struct EmitterGpu {
        uint32_t object_id{0};
        uint32_t emitter_index{0};  // index into VfxEffect::emitters
        uint32_t max_particles{0};
        uint32_t base_offset{0};
        ParticleBlendMode blend_mode{ParticleBlendMode::additive};
    };

    void rebuild_mega_buffers(uint32_t total_particles, uint32_t emitter_count);
    void init_emitter_ranges();
    void init_emitter_range(uint32_t from, uint32_t to);

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};

    // Compute pipeline (shared set layout for all 3 shaders)
    VkDescriptorSetLayout compute_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout compute_layout_{VK_NULL_HANDLE};
    VkPipeline emit_pipeline_{VK_NULL_HANDLE};
    VkPipeline sim_pipeline_{VK_NULL_HANDLE};
    VkPipeline indirect_pipeline_{VK_NULL_HANDLE};

    // Render pipeline
    VkDescriptorSetLayout render_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout render_layout_{VK_NULL_HANDLE};
    VkPipeline render_additive_{VK_NULL_HANDLE};
    VkPipeline render_alpha_{VK_NULL_HANDLE};

    // Descriptor pool + single sets
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet compute_set_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> render_sets_{};

    // Mega buffers
    std::unique_ptr<Buffer> mega_particle_buffer_;
    std::unique_ptr<Buffer> mega_counter_buffer_;
    std::unique_ptr<Buffer> mega_dead_list_buffer_;
    std::unique_ptr<Buffer> mega_indirect_buffer_;
    std::unique_ptr<Buffer> emitter_data_buffer_;  // HOST_VISIBLE, mapped
    std::unique_ptr<Buffer> module_buffer_;         // HOST_VISIBLE, per-emitter module data

    // Emitter tracking
    std::vector<EmitterGpu> emitters_;
    std::unordered_set<uint32_t> emitter_id_set_;
    std::vector<GpuEmitterData> emitter_data_cache_;

    // Capacity
    uint32_t total_particle_capacity_{0};
    uint32_t emitter_capacity_{0};

    // Cached layouts
    VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
    VkFormat color_format_{VK_FORMAT_UNDEFINED};
    VkFormat depth_format_{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits sample_count_{VK_SAMPLE_COUNT_1_BIT};
    VkSampler depth_sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout bindless_set_layout_{VK_NULL_HANDLE};

    uint32_t frame_seed_{0};
};

} // namespace fjell
