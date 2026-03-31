#pragma once

#include <array>
#include <vector>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace fjell {

struct DrawCommand;
class Device;
class DebugDraw;
class OffscreenPass;
class RenderQueue;
class RenderTargetPool;
class Scene;

// Shared descriptor set layouts needed by passes at pipeline creation time.
struct PassResources {
    VkDescriptorSetLayout global_set_layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout material_set_layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout draw_data_layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout meshlet_layout{VK_NULL_HANDLE};
    VkFormat depth_format{VK_FORMAT_UNDEFINED};
    VkFormat color_format{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits sample_count{VK_SAMPLE_COUNT_1_BIT};
};

class IndirectBatch;
class MegaBuffer;
class PipelineCache;
class PostProcessResources;

// Shared state that every pass can read during recording.
struct FrameContext {
    VkCommandBuffer cmd{VK_NULL_HANDLE};
    OffscreenPass* target{nullptr};
    const RenderQueue* queue{nullptr};
    VkDescriptorSet global_descriptor_set{VK_NULL_HANDLE};
    VkDescriptorSet bindless_set{VK_NULL_HANDLE};
    VkDescriptorSet draw_data_set{VK_NULL_HANDLE};
    glm::vec3 camera_position{0.0f};
    DebugDraw* debug_draw{nullptr};
    const Scene* scene{nullptr};
    float delta_time{0.0f};
    uint32_t selected_id{0};
    bool show_colliders{false};
    uint32_t frame_index{0};
    IndirectBatch* indirect_batch{nullptr};
    MegaBuffer* mega_buffer{nullptr};
    VkDescriptorSet meshlet_set{VK_NULL_HANDLE};
    bool use_mesh_shaders{false};
    PipelineCache* pipeline_cache{nullptr};

    // Two-pass occlusion: GPU-driven dispatch from culled visible buffer
    VkBuffer gpu_driven_indirect{VK_NULL_HANDLE}; // meshlet_instance_pass indirect cmds
    uint32_t gpu_driven_segment_size{0};           // instances per pipeline bin

    // Metallic/roughness G-buffer (resolved) for SSR material-aware blending
    VkImageView mr_view{VK_NULL_HANDLE};

    // SSR: previous frame color for reflection tracing
    VkImageView prev_frame_color_view{VK_NULL_HANDLE};

    // Hi-Z pyramid (shared, built by HizPass)
    VkImageView hiz_view{VK_NULL_HANDLE};
    VkSampler hiz_sampler{VK_NULL_HANDLE};
    uint32_t hiz_mip_count{0};

    // Forward+ light culling: tile light index SSBO for viz passes
    VkBuffer tile_light_ssbo{VK_NULL_HANDLE};
    VkDeviceSize tile_light_ssbo_size{0};

    // Transient render target pool
    RenderTargetPool* target_pool{nullptr};

    // Per-viewport post-process images
    PostProcessResources* pp_resources{nullptr};

    // Skip shadow pass for this viewport (e.g. camera preview uses main viewport's shadows)
    bool skip_shadows{false};

    // Flip tonemap output vertically so render-to-texture viewports produce
    // images with V=0 at bottom (standard texture convention).
    bool flip_tonemap_y{false};

    // Per-cascade shadow batches (indexed by cascade index)
    static constexpr uint32_t MAX_SHADOW_CASCADES = 3;
    std::array<IndirectBatch*, MAX_SHADOW_CASCADES> shadow_cascade_batches{};
    std::array<VkDescriptorSet, MAX_SHADOW_CASCADES> shadow_cascade_draw_data{};
    std::array<const std::vector<DrawCommand>*, MAX_SHADOW_CASCADES> shadow_cascade_commands{};
};

// Base class for a render pass. Each pass owns its pipeline(s) and
// knows how to record its commands into a command buffer.
class RenderPass {
public:
    virtual ~RenderPass() = default;

    RenderPass() = default;
    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;
    RenderPass(RenderPass&&) = delete;
    RenderPass& operator=(RenderPass&&) = delete;

    // Create GPU resources (pipelines, layouts).
    virtual void create(Device& device, const PassResources& resources) = 0;

    // Destroy GPU resources.
    virtual void destroy() = 0;

    // Record draw commands into the command buffer.
    virtual void record(const FrameContext& ctx) = 0;

    void recreate(Device& device, const PassResources& resources) {
        destroy();
        create(device, resources);
    }
};

} // namespace fjell
