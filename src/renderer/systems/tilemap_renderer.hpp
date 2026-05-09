#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/systems/render_subsystem.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

namespace fjell {

class Device;
class Project;
class ResourceManager;
class Scene;

class TilemapRenderer : public RenderSubsystem {
public:
    void create(Device& device, const PassResources& resources) override;
    void destroy() override;

    // RenderSubsystem lifecycle
    void update(const RenderSubsystemContext& ctx) override;
    void record_opaque(const FrameContext& ctx) override;
    void record_transparent(const FrameContext& ctx) override;
    [[nodiscard]] int transparent_order() const override { return -100; }

    // Resolve texture paths to bindless indices (called externally by editor)
    void sync_textures(Scene& scene, ResourceManager& resources, const Project* project = nullptr);

private:
    // Two pipeline buckets. Lit tilemaps render in the opaque pass with depth
    // write on so they act as 3D-occluding floors; unlit stays transparent
    // for the classic 2D-overlay path.
    enum class Bucket : uint8_t {
        UnlitTransparent = 0,
        LitOpaque        = 1,
        Count            = 2,
    };

    void update_impl(const Scene& scene, const glm::mat4& view_projection, uint32_t frame_index);
    void record_bucket(const FrameContext& ctx, Bucket bucket);

    struct GpuTile {
        glm::vec2 position; // 8
        glm::vec2 size;     // 8
        glm::vec4 uv_rect;  // 16
    };
    static_assert(sizeof(GpuTile) == 32);

    struct PushConstants {
        uint32_t texture_index;
        float z_depth;
        uint32_t normal_index;
        uint32_t flags;       // bit 0: lit
        float roughness;
        float metallic;
        float emission;
        float _pad;
    };

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkDescriptorSetLayout tile_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    std::array<VkPipeline, static_cast<size_t>(Bucket::Count)> pipelines_{};

    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> tile_ssbos_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> tile_sets_{};
    VkDescriptorPool pool_{VK_NULL_HANDLE};

    VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout bindless_set_layout_{VK_NULL_HANDLE};
    VkFormat color_format_{VK_FORMAT_UNDEFINED};
    VkFormat depth_format_{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits sample_count_{VK_SAMPLE_COUNT_1_BIT};

    // Per-frame tile count + which bucket the (single) drawn tilemap fell into.
    // Multi-tilemap support would split this into per-bucket ranges.
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> tile_counts_{};
    std::array<Bucket,   MAX_FRAMES_IN_FLIGHT> tile_buckets_{};
    std::array<PushConstants, MAX_FRAMES_IN_FLIGHT> push_constants_{};

    static constexpr uint32_t MAX_TILES = 16384;
    static constexpr VkDeviceSize SSBO_SIZE = MAX_TILES * sizeof(GpuTile);
};

} // namespace fjell
