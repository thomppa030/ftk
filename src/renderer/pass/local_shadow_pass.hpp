#pragma once

#include "gpu/owned.hpp"
#include "gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/uniform_types.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <vector>

namespace fjell {

class Scene;

// Atlas tile assignment for a single shadow-casting light.
struct ShadowTile {
    uint32_t x{0};          // pixel offset in atlas
    uint32_t y{0};
    uint32_t size{LOCAL_SHADOW_TILE_SIZE};
    glm::mat4 light_vp{1.0f};
    uint32_t light_index{0}; // index into the GPU light SSBO
    bool is_spot{true};
};

// Renders shadow maps for point and spot lights into a 2D depth atlas.
class LocalShadowPass : public RenderPass {
public:
    static constexpr uint32_t ATLAS_SIZE = LOCAL_SHADOW_ATLAS_SIZE;
    static constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    [[nodiscard]] VkImageView atlas_view() const { return atlas_view_; }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }

    // Spot light tiles assigned this frame (read by update_light_ssbo to fill shadow fields)
    [[nodiscard]] const std::vector<ShadowTile>& spot_tiles() const { return spot_tiles_; }

    // Point light tile blocks assigned this frame.
    // Each entry represents a 2x3 block of 6 face tiles for one point light.
    // Face layout within the block: faces 0-5 at (col % 2, col / 2) offsets.
    struct PointShadowBlock {
        uint32_t x{0};          // pixel offset of block origin in atlas
        uint32_t y{0};
        uint32_t tile_size{LOCAL_SHADOW_TILE_SIZE};
        uint32_t light_index{0};
        std::array<glm::mat4, 6> face_vp{}; // view-projection per cubemap face
        float near_plane{0.1F};
        float far_plane{1.0F};  // = light radius
    };
    [[nodiscard]] const std::vector<PointShadowBlock>& point_blocks() const { return point_blocks_; }

private:
    void create_atlas();
    void destroy_atlas();
    void assign_spot_tiles(const FrameContext& ctx);
    void assign_point_tiles(const FrameContext& ctx);

    VkDevice device_{VK_NULL_HANDLE};
    gpu::Device* gpu_device_{nullptr};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    // Atlas image
    VkImage atlas_image_{VK_NULL_HANDLE};
    VmaAllocation atlas_memory_{VK_NULL_HANDLE};
    VkImageView atlas_view_{VK_NULL_HANDLE};
    /// The atlas as the GPU interface's texture.
    gpu::Owned<gpu::Texture> atlas_texture_;
    VkSampler sampler_{VK_NULL_HANDLE};

    // Pipeline (reuses shadow.vert / depth_only.frag)
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkPipeline skinned_pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout draw_data_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout material_set_layout_{VK_NULL_HANDLE};

    // Per-frame tile assignments
    std::vector<ShadowTile> spot_tiles_;
    std::vector<PointShadowBlock> point_blocks_;
    /// Whether the pass has drawn into the atlas since it was made.
    bool written_{false};

    /// Whether any light casts a shadow this frame.
    [[nodiscard]] bool has_work() const { return !spot_tiles_.empty() || !point_blocks_.empty(); }

    // Cached scene pointer from prepare_frame
    Scene* active_scene_{nullptr};
};

} // namespace fjell
