#include "renderer/render_pipeline.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/thread_command_pools.hpp"
#include "core/thread_pool.hpp"

#include <algorithm>

namespace fjell {

const RenderPipeline::PassState RenderPipeline::default_state_{};

void RenderPipeline::register_pass(PassEntry entry) {
    pass_state_.try_emplace(entry.name); // create default state if not exists
    entries_.push_back(std::move(entry));
    sorted_ = false;
}

void RenderPipeline::remove_pass(const std::string& name) {
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                        [&](const PassEntry& e) { return e.name == name; }),
        entries_.end());
    pass_state_.erase(name);
}

void RenderPipeline::set_pass_enabled(const std::string& name, bool enabled) {
    pass_state_[name].enabled = enabled;
}

bool RenderPipeline::is_pass_enabled(const std::string& name) const {
    return state_for(name).enabled;
}

void RenderPipeline::set_pass_resources(const std::string& name,
                                         std::vector<std::string> reads,
                                         std::vector<std::string> writes) {
    auto& s = pass_state_[name];
    s.reads = std::move(reads);
    s.writes = std::move(writes);
}

bool RenderPipeline::is_resource_available(const std::string& resource) const {
    for (const auto& [name, state] : pass_state_) {
        if (!state.enabled) continue;
        for (const auto& w : state.writes) {
            if (w == resource) return true;
        }
    }
    return false;
}

RenderPipeline::PassState& RenderPipeline::state_for(const std::string& name) {
    return pass_state_[name];
}

const RenderPipeline::PassState& RenderPipeline::state_for(const std::string& name) const {
    auto it = pass_state_.find(name);
    if (it != pass_state_.end()) return it->second;
    return default_state_;
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

    // Each enabled entry adds itself to the graph with its image dependencies
    for (auto& entry : entries_) {
        if (!state_for(entry.name).enabled) continue;
        entry.build(graph, images);
    }

    graph.execute(cmd, &pool, &secondary_pools, frame_index);
}

} // namespace fjell
