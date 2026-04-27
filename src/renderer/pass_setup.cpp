#include "renderer/pass_setup.hpp"
#include "renderer/fjgraph_asset.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/pass_factory.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/pass/render_pass.hpp"

// Mesh shader passes are still constructed in C++ (conditional on device
// features). Everything else flows through PassFactory by class name.
#include "renderer/pass/cull_pass.hpp"
#include "renderer/pass/cull_viz_pass.hpp"
#include "renderer/pass/mesh_shader_depth_pass.hpp"
#include "renderer/pass/meshlet_cull_pass2.hpp"
#include "renderer/pass/meshlet_instance_pass.hpp"
#include "renderer/pass/meshlet_viz_pass.hpp"

namespace fjell {

Result<> create_passes_from_graph(const FjGraph& graph,
                                  PassRegistry& passes,
                                  GpuContext& gpu,
                                  const PassResources& resources) {
    auto& device = gpu.device_ref();

    for (const auto& entry : graph.passes) {
        auto pass = PassFactory::make(entry.class_name, entry.params);
        if (!pass) {
            return make_error("PassFactory::make returned null for '" +
                              entry.class_name +
                              "' — should have been caught at parse time");
        }

        auto create_result = pass->create(device, resources);
        if (!create_result) {
            return make_error("Pass '" + entry.name + "' (" + entry.class_name +
                              ") failed to create: " + create_result.error());
        }

        passes.add<RenderPass>(entry.name, std::move(pass));
    }

    if (gpu.use_mesh_shaders()) {
        if (!create_meshlet_passes(passes, gpu, resources)) {
            return make_error("Failed to create meshlet passes");
        }
    }

    passes.post_create_all(gpu);
    return {};
}

bool create_meshlet_passes(PassRegistry& passes, GpuContext& gpu,
                           const PassResources& resources) {
    auto& device = gpu.device_ref();

    auto add = [&](const std::string& name, auto pass) -> bool {
        using T = std::remove_reference_t<decltype(*pass)>;
        auto& ref = passes.add<T>(name, std::move(pass));
        return static_cast<bool>(ref.create(device, resources));
    };

    if (!add("meshlet_instance", std::make_unique<MeshletInstancePass>())) {
        return false;
    }

    auto pass2 = std::make_unique<MeshletCullPass2>();
    pass2->set_instance_pass(&passes.get<MeshletInstancePass>("meshlet_instance"));
    if (!add("meshlet_cull_pass2", std::move(pass2))) { return false; }

    return add("mesh_shader_depth", std::make_unique<MeshShaderDepthPass>())
        && add("meshlet_viz", std::make_unique<MeshletVizPass>())
        && add("cull", std::make_unique<CullPass>())
        && add("cull_viz", std::make_unique<CullVizPass>());
}

} // namespace fjell
