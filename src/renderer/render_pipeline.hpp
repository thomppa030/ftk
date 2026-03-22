#pragma once

#include "renderer/frame_images.hpp"
#include "renderer/frame_graph.hpp"

#include <functional>
#include <string>
#include <vector>

namespace fjell {

class OffscreenPass;
class ThreadPool;
class ThreadCommandPools;

/// A self-declaring pass registration: name, execution order, and a build
/// callback that adds the pass to the FrameGraph with its image dependencies.
struct PassEntry {
    std::string name;
    uint32_t order{0};

    /// Called during execute(). Must call graph.add_pass() with its image deps.
    using BuildFunc = std::function<void(FrameGraph& graph, const FrameImages& images)>;
    BuildFunc build;
};

/// Pass registry + frame graph execution. Passes register themselves with
/// their image dependencies; execute() builds and runs the frame graph.
class RenderPipeline {
public:
    void register_pass(PassEntry entry);
    void remove_pass(const std::string& name);

    /// Build the frame graph from registered passes and execute it.
    void execute(VkCommandBuffer cmd, OffscreenPass& target,
                 VkSampleCountFlagBits msaa, uint32_t active_sources,
                 ThreadPool& pool, ThreadCommandPools& secondary_pools,
                 uint32_t frame_index);

private:
    std::vector<PassEntry> entries_;
    bool sorted_{false};
};

} // namespace fjell
