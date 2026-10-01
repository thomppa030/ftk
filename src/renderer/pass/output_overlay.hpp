#pragma once

#include "ftk/gpu/render_encoder.hpp"
#include "ftk/gpu/sampler.hpp"

#include <optional>

namespace fjell {

class PassBuilder;
struct DeclareContext;
struct FrameContext;

// What the editor overlays (the grid, debug lines) share. They draw over the
// finished frame, the post pass's output, in display colour, so no post
// shader grades, outlines or blooms them. The output has no depth attachment:
// an overlay tests against the scene depth in its fragment shader
// (behind_scene() in fjell_overlay_depth.glsl), sampled through bind_depth().
//
// The post pass leaves the output for ImGui to sample. An overlay takes it
// back as a colour attachment and the graph returns it, so an overlay
// declares its access only in a frame it draws in: declare() and targets()
// agree on when that is.
class OutputOverlay {
public:
    /// Makes the sampler the scene depth is read through.
    void create(gpu::Device& device);

    /// Declare an overlay's accesses: the output written as a colour
    /// attachment and handed back to ImGui, the scene depth sampled. False,
    /// declaring nothing, when the viewport has no output to draw over.
    static bool declare(PassBuilder& builder, const DeclareContext& ctx);

    /// The output as the one colour target an overlay draws over, kept as it
    /// is beneath. None, and nothing is drawn, exactly when declare()
    /// declared nothing.
    [[nodiscard]] static std::optional<gpu::RenderTargets> targets(const FrameContext& ctx);

    /// Binds the scene depth where the overlay's shaders test against it
    /// (`scene_depth` in fjell_overlay_depth.glsl).
    void bind_depth(gpu::RenderEncoder& pass, const FrameContext& ctx) const;

private:
    gpu::Sampler depth_sampler_;
};

} // namespace fjell
