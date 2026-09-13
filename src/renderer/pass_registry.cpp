#include "renderer/pass_registry.hpp"

namespace fjell {

static void register_one(RenderPass* pass, RenderPipeline& pipeline) {
    auto s = pass->setup();

    // A pass that returns no name from setup() is recorded by another pass
    // rather than by the pipeline, so it gets no entry of its own.
    if (s.name.empty()) return;

    pipeline.register_pass({
        .name = s.name,
        .order = PassEntry::make_order(s.phase, s.sub_order),
        .pass = pass,
    });
}

void PassRegistry::register_with_pipeline(RenderPipeline& pipeline) {
    for (const auto& name : order_) {
        auto it = passes_.find(name);
        if (it == passes_.end()) continue;
        register_one(it->second.get(), pipeline);
    }
}

void PassRegistry::register_pass_with_pipeline(const std::string& name,
                                                RenderPipeline& pipeline) {
    auto it = passes_.find(name);
    if (it == passes_.end()) return;
    register_one(it->second.get(), pipeline);
}

} // namespace fjell
