#include "renderer/pass/viz_meshes.hpp"

#include "gpu/render_encoder.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/model.hpp"

#include <vector>

namespace fjell {

void draw_viz_meshes(const FrameContext& ctx, gpu::RenderEncoder& pass) {
    if (ctx.queue == nullptr) return;
    // Models still record their own draws from the mega buffer, into the
    // command buffer the encoder's scope is open on.
    if (ctx.mega_buffer) ctx.mega_buffer->bind(ctx.cmd);
    auto draw_list = [&](const std::vector<DrawCommand>& list) {
        for (const auto& dc : list) {
            if (dc.skinned) continue; // TODO(skinned-viz): add skinned pipeline variant
            pass.push(dc.world_matrix);
            dc.model->draw(ctx.cmd);
        }
    };
    draw_list(ctx.queue->commands());
    draw_list(ctx.queue->transparent_commands());
}

} // namespace fjell
