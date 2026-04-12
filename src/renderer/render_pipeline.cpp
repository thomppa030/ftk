#include "renderer/render_pipeline.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/gpu/thread_command_pools.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"

#include <algorithm>
#include <queue>

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
                              uint32_t frame_index, ResourceRegistry& registry) {
    FJELL_PROFILE_SCOPE_N("render_pipeline_execute");

    // Topological sort by resource dependencies, order as tiebreaker
    if (!sorted_) {
        size_t n = entries_.size();

        // Build adjacency: if pass i writes resource R and pass j reads R, add edge i→j
        std::vector<std::vector<size_t>> adj(n);
        std::vector<uint32_t> in_degree(n, 0);

        // Map resource name → indices of passes that write it
        std::unordered_map<std::string, std::vector<size_t>> writers;
        for (size_t i = 0; i < n; ++i) {
            const auto& s = state_for(entries_[i].name);
            for (const auto& w : s.writes) {
                writers[w].push_back(i);
            }
        }

        // For each pass that reads a resource, add edges from all its writers
        // Only add edge if the writer has a lower order (respects intended ordering
        // when multiple passes write the same resource in sequence)
        for (size_t j = 0; j < n; ++j) {
            const auto& s = state_for(entries_[j].name);
            for (const auto& r : s.reads) {
                auto it = writers.find(r);
                if (it == writers.end()) continue;
                for (size_t i : it->second) {
                    if (i == j) continue;
                    // Only add edge from writer → reader if writer has lower order
                    if (entries_[i].order < entries_[j].order) {
                        adj[i].push_back(j);
                        in_degree[j]++;
                    }
                }
            }
        }

        // Kahn's algorithm with priority queue (order as tiebreaker)
        auto cmp = [this](size_t a, size_t b) {
            return entries_[a].order > entries_[b].order; // min-heap
        };
        std::priority_queue<size_t, std::vector<size_t>, decltype(cmp)> pq(cmp);

        for (size_t i = 0; i < n; ++i) {
            if (in_degree[i] == 0) pq.push(i);
        }

        std::vector<PassEntry> sorted;
        sorted.reserve(n);
        while (!pq.empty()) {
            size_t idx = pq.top();
            pq.pop();
            sorted.push_back(std::move(entries_[idx]));
            for (size_t next : adj[idx]) {
                if (--in_degree[next] == 0) pq.push(next);
            }
        }

        entries_ = std::move(sorted);
        sorted_ = true;
    }

    // Register images with the frame graph and populate the resource registry
    FrameGraph graph;
    bool has_msaa = (msaa != VK_SAMPLE_COUNT_1_BIT);

    auto color_id = graph.register_image(target.color_image());
    auto msaa_color_id = color_id;
    if (has_msaa) {
        msaa_color_id = graph.register_image(target.msaa_color_image());
    }

    auto depth_id = graph.register_image(target.depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT);
    auto msaa_depth_id = depth_id;
    if (has_msaa) {
        msaa_depth_id = graph.register_image(target.msaa_depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    auto mr_id = graph.register_image(target.mr_image());
    auto msaa_mr_id = mr_id;
    if (has_msaa) {
        msaa_mr_id = graph.register_image(target.msaa_mr_image());
    }

    auto normal_id = graph.register_image(target.normal_image());
    auto msaa_normal_id = normal_id;
    if (has_msaa) {
        msaa_normal_id = graph.register_image(target.msaa_normal_image());
    }

    auto screen_color_id = graph.register_image(target.screen_color_image());
    auto distortion_id = graph.register_image(target.distortion_image());

    graph.begin_frame();

    // Populate resource registry (bridge string names to FrameGraph IDs + views)
    registry.register_image("color", {color_id, target.color_view()});
    registry.register_image("msaa_color", {msaa_color_id, target.msaa_color_view()});
    registry.register_image("depth", {depth_id, target.depth_view()});
    registry.register_image("msaa_depth", {msaa_depth_id, target.msaa_depth_view()});
    registry.register_image("mr", {mr_id, target.mr_view()});
    registry.register_image("msaa_mr", {msaa_mr_id, target.msaa_mr_view()});
    registry.register_image("normal", {normal_id, target.normal_view()});
    registry.register_image("msaa_normal", {msaa_normal_id, target.msaa_normal_view()});
    registry.register_image("screen_color", {screen_color_id, target.screen_color_view()});
    registry.register_image("distortion", {distortion_id, target.distortion_view()});

    // Each enabled entry adds itself to the graph with its image dependencies
    for (auto& entry : entries_) {
        if (!state_for(entry.name).enabled) continue;
        entry.build(graph, registry);
    }

    graph.execute(cmd, &pool, &secondary_pools, frame_index);
}

} // namespace fjell
