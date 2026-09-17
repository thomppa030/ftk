#pragma once

#include "renderer/gpu/buffer.hpp"

#include <memory>
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

// What has moved through the world lately, and how fast: a small top-down map
// over a square that follows the camera, splatted each frame by whatever is
// moving and fading back to still. Bound globally as "motion_field" for
// anything that should react to movement — fog stirred by a runner, grass bent
// aside, a track through snow.
//
// A texel holds the velocity on the ground plane, how high the movement
// reached, and how recently it happened. **Decay belongs to the reader**: the
// map records when, and each consumer decides what that is worth to it,
// because grass springs back in a moment and fog settles over seconds.
//
// The square is snapped to whole texels, so what the map holds stands still on
// the ground as the camera moves. A map that re-centred every frame would
// resample its whole contents and shimmer.
class MotionFieldPass : public RenderPass {
public:
    static constexpr uint32_t RESOLUTION = 512;
    static constexpr float EXTENT_METRES = 64.0f;
    /// rg = velocity on the ground plane (m/s), b = height reached above the
    /// map's floor, a = recency, 1 at the moment of passing.
    static constexpr VkFormat FIELD_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
    /// How long the map itself remembers, in seconds. Long enough for the
    /// slowest consumer; a faster one fades its own reading sooner.
    static constexpr float MEMORY_SECONDS = 4.0f;
    /// Past this many sources in a frame the rest are dropped rather than
    /// growing the buffer mid-frame.
    static constexpr uint32_t MAX_SOURCES = 64;
    /// Below this, in metres per second, a source counts as standing still:
    /// it keeps its own ground marked but does not stir anything new.
    static constexpr float STILL_SPEED = 0.05f;
    /// How wide a mark a source leaves, in multiples of its own width. Air a
    /// body pushes through is disturbed well beyond the body itself, and a
    /// mark the width of the body is a line rather than a wake.
    static constexpr float WAKE_WIDTH = 3.0f;
    /// How much of a texel bleeds into its neighbours each second. This is
    /// what opens a wake out behind whatever left it instead of leaving the
    /// ribbon it was painted as.
    static constexpr float SPREAD_PER_SECOND = 0.3f;
    /// How hard a wake rolls as it opens out, in metres per second. Spreading
    /// alone only ever smooths — this is what turns the air instead, so a wake
    /// breaks into eddies rather than softening into a band.
    static constexpr float SWIRL_METRES_PER_SECOND = 0.6f;

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    /// Whether the map holds anything worth reading: something has moved
    /// recently enough not to have faded. A still scene pays nothing.
    [[nodiscard]] bool active() const { return active_; }
    /// World position → map uv, for the environment UBO.
    [[nodiscard]] const glm::mat4& world_to_uv() const { return world_to_uv_; }

    /// For the viewport's debug source.
    [[nodiscard]] VkDescriptorSet imgui_texture(VkSampler sampler);

private:
    /// One moving thing, as the splat shader reads it.
    struct GpuSource {
        alignas(16) glm::vec4 position_radius;  // xyz world, w metres
        alignas(16) glm::vec4 velocity_height;  // xyz m/s, w metres reached
        alignas(16) glm::vec4 strength;         // x scales the mark left
    };

    struct PushConstants {
        glm::vec4 map_origin_extent;   // xy min corner on the ground, z extent, w texel size
        glm::vec4 scroll_decay;        // xy texels scrolled since last frame, z decay, w dt
        uint32_t source_count;
        uint32_t resolution;
        float spread;
        float wake_width;
        float swirl;
        float time;
    };

    bool active_{false};
    /// Where the map's min corner sits this frame and sat last, so the scroll
    /// between them is a whole number of texels rather than a resample.
    glm::vec2 origin_{0.0f};
    glm::vec2 prev_origin_{0.0f};
    bool first_frame_{true};
    glm::mat4 world_to_uv_{1.0f};
    /// The map's floor, so a height in the blue channel is above something
    /// rather than an absolute the consumer has to guess at.
    float floor_y_{0.0f};

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};

    // Two maps, ping-ponged. The pass reads the previous frame's map at an
    // offset and writes this frame's, which a single image could not do: one
    // invocation's read and another's write would race over the same texel.
    struct Map {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
    };
    Map maps_[2];
    uint32_t cur_{0};
    VkSampler sampler_{VK_NULL_HANDLE};

    [[nodiscard]] uint32_t prev_index() const { return 1 - cur_; }

    std::unique_ptr<Buffer> source_buffer_;
    uint32_t source_count_{0};
    /// How many of those are actually moving. A still scene settles; a scene
    /// of standing characters is still a still scene.
    uint32_t moving_count_{0};
    float still_seconds_{0.0f};
    bool settled_{false};
    /// Seconds the map has been running, for the curl field to creep by.
    float elapsed_{0.0f};

    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    VkDescriptorSet imgui_set_{VK_NULL_HANDLE};
};

} // namespace fjell
