#pragma once

#include "renderer/frame_graph.hpp"
#include "renderer/resource_registry.hpp"

#include <functional>
#include <string>
#include <unordered_map>
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
    using BuildFunc = std::function<void(FrameGraph& graph, ResourceRegistry& registry)>;
    BuildFunc build;
};

/// Pass registry + frame graph execution. Passes register themselves with
/// their image dependencies; execute() builds and runs the frame graph.
class RenderPipeline {
public:
    void register_pass(PassEntry entry);
    void remove_pass(const std::string& name);

    /// Enable or disable a pass by name. Disabled passes are skipped during execute().
    void set_pass_enabled(const std::string& name, bool enabled);

    /// Query whether a pass is enabled.
    [[nodiscard]] bool is_pass_enabled(const std::string& name) const;

    /// Declare which named resources a pass reads and writes.
    void set_pass_resources(const std::string& name,
                            std::vector<std::string> reads,
                            std::vector<std::string> writes);

    /// Check whether a named resource has at least one enabled producer.
    [[nodiscard]] bool is_resource_available(const std::string& resource) const;

    /// Build the frame graph from registered passes and execute it.
    void execute(VkCommandBuffer cmd, OffscreenPass& target,
                 VkSampleCountFlagBits msaa, uint32_t active_sources,
                 ThreadPool& pool, ThreadCommandPools& secondary_pools,
                 uint32_t frame_index);

private:
    struct PassState {
        bool enabled{true};
        std::vector<std::string> reads;
        std::vector<std::string> writes;
    };

    [[nodiscard]] PassState& state_for(const std::string& name);
    [[nodiscard]] const PassState& state_for(const std::string& name) const;

    std::vector<PassEntry> entries_;
    std::unordered_map<std::string, PassState> pass_state_;
    bool sorted_{false};

    static const PassState default_state_;
};

} // namespace fjell
