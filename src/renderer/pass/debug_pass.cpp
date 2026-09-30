#include "renderer/pass/debug_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/debug_draw.hpp"
#include "renderer/pass/post_pass.hpp"
#include "renderer/pass_builder.hpp"

#include <glm/glm.hpp>

namespace fjell {

Result<> DebugPass::create(const PassResources& resources) {
    gpu::Device& device = *resources.device;
    overlay_.create(device);

    // Lines over the post pass's single-sampled output. No depth attachment:
    // the fragment shader tests against the scene depth. The line colour
    // replaces what is there; the output's alpha is left opaque, since ImGui
    // blends the viewport image by it.
    FJELL_TRY(pipeline_, device.create(gpu::GraphicsPipelineDesc{
                             .vertex = "shaders/debug.vert",
                             .fragment = "shaders/debug.frag",
                             .vertex_layout = {.stride = sizeof(glm::vec3),
                                               .attributes = {{.location = 0,
                                                               .format = gpu::VertexFormat::float3}}},
                             .topology = gpu::Topology::lines,
                             .color = {{.format = PostPass::OUTPUT_FORMAT,
                                        .write = gpu::Channel::r | gpu::Channel::g | gpu::Channel::b}},
                             .shared = {resources.globals},
                             .name = "debug",
                         }));

    FJELL_GFX_DEBUG("Debug pass pipeline created");
    return {};
}

void DebugPass::destroy() {
    pipeline_.reset();
}

PassSetup DebugPass::setup() const {
    return {
        .name = "debug",
        .phase = RenderPhase::PostProcess,
        .sub_order = 320, // over the post pass's output (300) and the grid
                          // (310), so no post shader grades or outlines it
    };
}

void DebugPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!ctx.debug_lines) return;
    OutputOverlay::declare(builder, ctx);
}

void DebugPass::record(const FrameContext& ctx) {
    if (!ctx.debug_draw || ctx.debug_draw->empty()) return;
    const auto targets = OutputOverlay::targets(ctx);
    if (!targets) return;

    FJELL_PROFILE_SCOPE_N("record_debug");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("Viz: Debug");
    auto pass = cmd.render(*targets);
    pass.set_pipeline(pipeline_);
    pass.bind(ctx.globals);
    overlay_.bind_depth(pass, ctx);
    ctx.debug_draw->draw(pass);
}

} // namespace fjell
