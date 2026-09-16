#pragma once

#include "renderer/frustum.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

struct EnvironmentState;

// What stands between a point of ground and the sky. A depth-only render
// straight down over a square that follows the camera, so anything under a
// roof, a bridge or a tree canopy is known to be sheltered. Two things read
// it: rain and snow particles, which die where they meet the shelter rather
// than falling through it, and lit surfaces, whose wetness and snow scale by
// how open to the sky they are.
//
// The map only exists while the weather needs it — while something is
// falling or the world is still wet — so a clear scene pays nothing for it.
class OverheadDepthPass : public RenderPass {
public:
    static constexpr uint32_t RESOLUTION = 512;
    static constexpr float EXTENT_METRES = 64.0f;
    /// How far above the camera the map looks down from, and how far below it
    /// reaches. Anything outside that slab is not shelter.
    static constexpr float HEIGHT_ABOVE = 40.0f;
    static constexpr float DEPTH_BELOW = 40.0f;
    static constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    /// Whether the map holds anything this frame.
    [[nodiscard]] bool active() const { return active_; }
    /// World position → map uv and the depth to compare against.
    [[nodiscard]] const glm::mat4& world_to_map() const { return world_to_map_; }

    /// The volume the map covers, for the gatherer to cull shelter against.
    [[nodiscard]] AABB reach_bounds(const glm::vec3& camera_position) const;

    /// Whether this environment wants the map at all: something is falling,
    /// or the world is still wet or snowed on. The gatherer asks before this
    /// pass has prepared its frame, so the answer comes from the environment
    /// rather than from the pass's own state.
    [[nodiscard]] static bool needed_by(const EnvironmentState& environment);

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

    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    VkQueue graphics_queue_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};

    glm::mat4 world_to_map_{1.0f};   // world → clip, and so → uv and depth
    bool active_{false};
    bool cleared_{false};
};

} // namespace fjell
