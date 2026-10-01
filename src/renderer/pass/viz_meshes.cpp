#include "renderer/pass/viz_meshes.hpp"

#include "ftk/gpu/render_encoder.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/model.hpp"

#include <vector>

namespace fjell {

void draw_viz_meshes(const FrameContext& ctx, ftk::gpu::RenderEncoder& pass) {
    if (ctx.queue == nullptr) return;
    if (ctx.mega_buffer == nullptr || ctx.indirect_batch == nullptr) return;
    ctx.mega_buffer->bind(pass);
    // Each command's material slot at the LOD the batch draws it at, as the
    // scene shows it.
    auto draw_list = [&](const std::vector<DrawCommand>& list) {
        for (const auto& dc : list) {
            if (dc.skinned) continue; // TODO(skinned-viz): add skinned pipeline variant
            pass.push(dc.world_matrix);
            const uint32_t lod = ctx.indirect_batch->lod_for(*dc.model, dc.world_matrix);
            dc.model->for_each_entry(lod, dc.material_slot, [&](const MegaMeshEntry& entry) {
                pass.draw_indexed(entry.index_count, 1, entry.first_index,
                                  static_cast<int32_t>(entry.vertex_offset), 0);
            });
        }
    };
    draw_list(ctx.queue->commands());
    draw_list(ctx.queue->transparent_commands());
}

} // namespace fjell
