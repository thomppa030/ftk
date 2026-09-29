#pragma once

#include "core/result.hpp"
#include "gpu/render_encoder.hpp"
#include "gpu/sampler.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>

namespace fjell {

class PassBuilder;
struct DeclareContext;
struct FrameContext;

// What the editor overlays (the grid, debug lines) share. They draw over the
// finished frame, the post pass's output, in display colour, so no post
// shader grades, outlines or blooms them. The output has no depth attachment:
// an overlay tests against the scene depth in its fragment shader
// (behind_scene() in fjell_overlay_depth.glsl), sampled through the set this
// makes.
//
// The post pass leaves the output for ImGui to sample. An overlay takes it
// back as a colour attachment, draws, and returns it the same way, so it
// declares its access only in a frame it draws in, and a record() that
// begin()s always end()s.
class OutputOverlay {
public:
    /// The set index an overlay's pipeline layout gives the scene depth:
    /// binding 0, sampled nearest. Set 0 is the global UBO.
    static constexpr uint32_t DEPTH_SET = 1;

    Result<> create(VkDevice device);
    /// Makes the sampler the scene depth is read through, for bind_depth().
    void create(gpu::Device& device);
    void destroy();

    [[nodiscard]] VkDescriptorSetLayout depth_set_layout() const { return depth_set_layout_; }

    /// Declare an overlay's accesses: the output written as a colour
    /// attachment and handed back to ImGui, the scene depth sampled. False,
    /// declaring nothing, when the viewport has no output to draw over.
    static bool declare(PassBuilder& builder, const DeclareContext& ctx);

    /// Begin rendering over the output, with the viewport and scissor
    /// covering it, and bind the scene depth at DEPTH_SET of `layout`. False,
    /// recording nothing, exactly when declare() declared nothing; when true,
    /// end() must follow.
    bool begin(const FrameContext& ctx, VkPipelineLayout layout) const;

    /// End rendering and hand the output back to ImGui.
    void end(const FrameContext& ctx) const;

    /// The output as the one colour target an overlay draws over, kept as it
    /// is beneath. None, and nothing is drawn, exactly when declare()
    /// declared nothing.
    [[nodiscard]] static std::optional<gpu::RenderTargets> targets(const FrameContext& ctx);

    /// Binds the scene depth where the overlay's shaders test against it
    /// (`scene_depth` in fjell_overlay_depth.glsl).
    void bind_depth(gpu::RenderEncoder& pass, const FrameContext& ctx) const;

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout depth_set_layout_{VK_NULL_HANDLE};
    gpu::Sampler depth_sampler_;
};

} // namespace fjell
