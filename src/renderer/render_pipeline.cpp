#include "renderer/render_pipeline.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/thread_command_pools.hpp"
#include "core/thread_pool.hpp"

#include <algorithm>

namespace fjell {

void RenderPipeline::register_pass(PassEntry entry) {
    entries_.push_back(std::move(entry));
    sorted_ = false;
}

void RenderPipeline::remove_pass(const std::string& name) {
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                        [&](const PassEntry& e) { return e.name == name; }),
        entries_.end());
}

void RenderPipeline::execute(VkCommandBuffer cmd, OffscreenPass& target,
                              VkSampleCountFlagBits msaa, uint32_t /*active_sources*/,
                              ThreadPool& pool, ThreadCommandPools& secondary_pools,
                              uint32_t frame_index) {
    // Sort entries by order if needed
    if (!sorted_) {
        std::sort(entries_.begin(), entries_.end(),
                  [](const PassEntry& a, const PassEntry& b) {
                      return a.order < b.order;
                  });
        sorted_ = true;
    }

    // Register images with the frame graph
    FrameGraph graph;
    FrameImages images;
    images.color_id = graph.register_image(target.color_image());
    images.msaa_color_id = images.color_id;
    images.has_msaa = (msaa != VK_SAMPLE_COUNT_1_BIT);
    if (images.has_msaa) {
        images.msaa_color_id = graph.register_image(target.msaa_color_image());
    }
    images.depth_id = graph.register_image(target.depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT);
    images.msaa_depth_id = images.depth_id;
    if (images.has_msaa) {
        images.msaa_depth_id = graph.register_image(target.msaa_depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    images.mr_id = graph.register_image(target.mr_image());
    images.msaa_mr_id = images.mr_id;
    if (images.has_msaa) {
        images.msaa_mr_id = graph.register_image(target.msaa_mr_image());
    }
    graph.begin_frame();

    // Each entry adds itself to the graph with its image dependencies
    for (auto& entry : entries_) {
        entry.build(graph, images);
    }

    graph.execute(cmd, &pool, &secondary_pools, frame_index);
}

} // namespace fjell
