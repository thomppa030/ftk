#include "renderer/pass_registry.hpp"
#include "renderer/resource_registry.hpp"

namespace fjell {

static void register_one(RenderPass* pass, RenderPipeline& pipeline, FrameContext& ctx) {
    auto s = pass->setup();

    // Passes with empty setup name are sub-passes (sky, grid, debug)
    // that are recorded by their parent — skip pipeline registration.
    if (s.name.empty()) return;

    // Extract string reads/writes for dependency ordering
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    reads.reserve(s.reads.size());
    writes.reserve(s.writes.size());
    for (const auto& r : s.reads) reads.push_back(r.name);
    for (const auto& w : s.writes) writes.push_back(w.name);

    // Register the pass entry — build delegates to the pass
    pipeline.register_pass({
        .name = s.name,
        .order = PassEntry::make_order(s.phase, s.sub_order),
        .build = [pass, &ctx](FrameGraph& graph, ResourceRegistry& registry) {
            pass->build(graph, registry, ctx);
        },
        .pass = pass,
    });

    // Register string dependencies for topological ordering
    if (!reads.empty() || !writes.empty()) {
        pipeline.set_pass_resources(s.name, std::move(reads), std::move(writes));
    }
}

void PassRegistry::register_with_pipeline(RenderPipeline& pipeline, FrameContext& ctx) {
    for (const auto& name : order_) {
        auto it = passes_.find(name);
        if (it == passes_.end()) continue;
        register_one(it->second.get(), pipeline, ctx);
    }
}

void PassRegistry::register_pass_with_pipeline(const std::string& name,
                                                RenderPipeline& pipeline, FrameContext& ctx) {
    auto it = passes_.find(name);
    if (it == passes_.end()) return;
    register_one(it->second.get(), pipeline, ctx);
}

} // namespace fjell
