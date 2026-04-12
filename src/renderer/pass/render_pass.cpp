#include "renderer/pass/render_pass.hpp"
#include "renderer/resource_registry.hpp"

namespace fjell {

void RenderPass::build(FrameGraph& graph, ResourceRegistry& registry,
                       const FrameContext& ctx) {
    auto s = setup();
    if (s.name.empty()) return;

    // Resolve named resources to FrameGraph image IDs
    std::vector<std::pair<uint32_t, ImageUsage>> uses;
    uses.reserve(s.reads.size() + s.writes.size());
    for (const auto& r : s.reads) {
        auto* img = registry.find_image(r.name);
        if (img) {
            uses.emplace_back(img->frame_graph_id, r.usage);
        }
    }
    for (const auto& w : s.writes) {
        auto* img = registry.find_image(w.name);
        if (img) {
            uses.emplace_back(img->frame_graph_id, w.usage);
        }
    }

    // Capture this pass + a copy of FrameContext for the record callback
    graph.add_pass(s.name, [this, ctx](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        record(local);
    }, std::move(uses));
}

} // namespace fjell
