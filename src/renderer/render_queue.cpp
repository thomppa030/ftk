#include "renderer/render_queue.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/render_stats.hpp"

#include <algorithm>

namespace fjell {

void RenderQueue::clear() {
    opaque_.clear();
    transparent_.clear();
}

void RenderQueue::push(const DrawCommand& cmd) {
    if (cmd.transparent) {
        transparent_.push_back(cmd);
    } else {
        opaque_.push_back(cmd);
    }
}

void RenderQueue::sort(const glm::vec3& camera_pos) {
    // Opaque: sort by material to minimize descriptor rebinds
    std::sort(opaque_.begin(), opaque_.end(),
              [](const DrawCommand& a, const DrawCommand& b) {
                  return a.sort_key < b.sort_key;
              });

    // Transparent: compute distance and sort back-to-front
    for (auto& dc : transparent_) {
        glm::vec3 obj_pos = glm::vec3(dc.world_matrix[3]);
        dc.camera_distance = glm::length(camera_pos - obj_pos);
    }
    std::sort(transparent_.begin(), transparent_.end(),
              [](const DrawCommand& a, const DrawCommand& b) {
                  return a.camera_distance > b.camera_distance;
              });
}

static void execute_list(const std::vector<DrawCommand>& list,
                         VkCommandBuffer cmd, VkPipelineLayout layout,
                         RenderStats* stats) {
    VkDescriptorSet bound_set = VK_NULL_HANDLE;

    for (const auto& dc : list) {
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

void RenderQueue::execute(VkCommandBuffer cmd, VkPipelineLayout layout,
                          RenderStats* stats) const {
    execute_list(opaque_, cmd, layout, stats);
}

void RenderQueue::execute_transparent(VkCommandBuffer cmd, VkPipelineLayout layout,
                                      RenderStats* stats) const {
    execute_list(transparent_, cmd, layout, stats);
}

uint64_t RenderQueue::make_sort_key(uint32_t material_id, uint32_t mesh_id) {
    return (static_cast<uint64_t>(material_id) << 32) | mesh_id;
}

} // namespace fjell
