#include "renderer/pass_setup.hpp"
#include "renderer/fjgraph_asset.hpp"
#include "renderer/gpu_context.hpp"
#include <gpu/device.hpp>
#include "renderer/pass_factory.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/render_pipeline.hpp"
#include "renderer/pass/render_pass.hpp"

namespace fjell {

// A pass's `enabled_if` names a device capability rather than a boolean the
// engine tracks generically — there's no feature registry yet, just this
// switch. Add a case here when a pass needs to gate on something the device
// may not support (mirrors the mesh-shader gate below).
static bool feature_available(const std::string& key, const gpu::Caps& caps) {
    if (key == "mesh_shaders") { return caps.mesh_shaders; }
    return true;
}

Result<> create_passes_from_graph(const FjGraph& graph,
                                  PassRegistry& passes,
                                  RenderPipeline& pipeline,
                                  GpuContext& gpu,
                                  const PassResources& resources) {
    const gpu::Caps& caps = resources.device->caps();

    for (const auto& entry : graph.passes) {
        // A pass whose device feature is missing is skipped rather than
        // constructed and left disabled: building it at all would create a
        // pipeline with mesh/task shader stages the device can't run —
        // vkCreateGraphicsPipelines fails outright when the extension isn't
        // enabled, not just at draw time.
        if (entry.enabled_if && !feature_available(*entry.enabled_if, caps)) {
            continue;
        }

        auto pass = PassFactory::make(entry.class_name, entry.params);
        if (!pass) {
            return make_error("PassFactory::make returned null for '" +
                              entry.class_name +
                              "' — should have been caught at parse time");
        }

        auto create_result = pass->create(gpu.device_ref(), resources);
        if (!create_result) {
            return make_error("Pass '" + entry.name + "' (" + entry.class_name +
                              ") failed to create: " + create_result.error());
        }

        passes.add<RenderPass>(entry.name, std::move(pass));
    }

    passes.post_create_all(gpu);
    passes.register_with_pipeline(pipeline);
    apply_enabled_flags(graph, pipeline);
    return {};
}

void apply_enabled_flags(const FjGraph& graph, RenderPipeline& pipeline) {
    for (const auto& entry : graph.passes) {
        pipeline.set_pass_enabled(entry.name, entry.enabled);
    }
}

} // namespace fjell
