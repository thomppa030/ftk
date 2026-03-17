#include "renderer/render_queue.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/render_stats.hpp"

#include <algorithm>

namespace fjell {

void RenderQueue::clear() {
    commands_.clear();
}

void RenderQueue::push(const DrawCommand& cmd) {
    commands_.push_back(cmd);
}

void RenderQueue::sort() {
    std::sort(commands_.begin(), commands_.end(),
              [](const DrawCommand& a, const DrawCommand& b) {
                  return a.sort_key < b.sort_key;
              });
}

void RenderQueue::execute(VkCommandBuffer cmd, VkPipelineLayout layout,
                          RenderStats* stats) const {
    VkDescriptorSet bound_set = VK_NULL_HANDLE;

    for (const auto& dc : commands_) {
        vkCmdPushConstants(cmd, layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PbrPushConstants), &dc.push);

        if (dc.material_set != VK_NULL_HANDLE && dc.material_set != bound_set) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                    1, 1, &dc.material_set, 0, nullptr);
            bound_set = dc.material_set;
        }

        if (stats) {
            stats->draw_calls += dc.model->mesh_count();
            stats->triangle_count += dc.model->total_indices() / 3;
        }

        dc.model->draw(cmd);
    }
}

uint64_t RenderQueue::make_sort_key(uint32_t material_id, uint32_t mesh_id) {
    return (static_cast<uint64_t>(material_id) << 32) | mesh_id;
}

} // namespace fjell
