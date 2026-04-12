#include "renderer/pass_registry.hpp"
#include "renderer/resource_registry.hpp"

namespace fjell {

void PassRegistry::register_with_pipeline(RenderPipeline& pipeline, FrameContext& ctx) {
    for (const auto& name : order_) {
        auto it = passes_.find(name);
        if (it == passes_.end()) continue;

        auto* pass = it->second.get();
        auto s = pass->setup();

        // Passes with empty setup name are sub-passes (sky, grid, debug)
        // that are recorded by their parent — skip pipeline registration.
        if (s.name.empty()) continue;

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
            }
        });

        // Register string dependencies for topological ordering
        if (!reads.empty() || !writes.empty()) {
            pipeline.set_pass_resources(s.name, std::move(reads), std::move(writes));
        }
    }
}

} // namespace fjell
