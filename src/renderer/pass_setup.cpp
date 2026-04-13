#include "renderer/pass_setup.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/pass/render_pass.hpp"

// All pass types — concentrated here instead of vulkan_context.hpp
#include "renderer/pass/bloom_pass.hpp"
#include "renderer/pass/cull_pass.hpp"
#include "renderer/pass/cull_viz_pass.hpp"
#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/pass/debug_pass.hpp"
#include "renderer/pass/depth_prepass.hpp"
#include "renderer/pass/depth_viz_pass.hpp"
#include "renderer/pass/distortion_pass.hpp"
#include "renderer/pass/grid_pass.hpp"
#include "renderer/pass/gtao_pass.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/pass/light_cull_pass.hpp"
#include "renderer/pass/light_viz_pass.hpp"
#include "renderer/pass/mesh_shader_depth_pass.hpp"
#include "renderer/pass/meshlet_cull_pass2.hpp"
#include "renderer/pass/meshlet_instance_pass.hpp"
#include "renderer/pass/meshlet_viz_pass.hpp"
#include "renderer/pass/normal_pass.hpp"
#include "renderer/pass/opaque_pass.hpp"
#include "renderer/pass/overdraw_pass.hpp"
#include "renderer/pass/scene_sdf_pass.hpp"
#include "renderer/pass/screen_color_copy_pass.hpp"
#include "renderer/pass/shadow_pass.hpp"
#include "renderer/pass/sky_cubemap_pass.hpp"
#include "renderer/pass/sky_pass.hpp"
#include "renderer/pass/ssr_pass.hpp"
#include "renderer/pass/subsystem_compute_pass.hpp"
#include "renderer/pass/tonemap_pass.hpp"
#include "renderer/pass/transparent_pass.hpp"
#include "renderer/pass/uv_viz_pass.hpp"
#include "renderer/pass/volumetric_fog_pass.hpp"
#include "renderer/pass/wireframe_pass.hpp"

#include "core/log.hpp"

#include <stdexcept>

namespace fjell {

void create_all_passes(PassRegistry& passes, GpuContext& gpu,
                       const PassResources& resources) {
    auto& device = gpu.device_ref();

    // Helper: create, register, and initialize a pass in one call
    auto add = [&](const std::string& name, auto pass) -> auto& {
        using T = std::remove_reference_t<decltype(*pass)>;
        auto& ref = passes.add<T>(name, std::move(pass));
        auto r = ref.create(device, resources);
        if (!r) throw std::runtime_error(r.error());
        return ref;
    };

    // ── Core geometry passes ───────────────────────────────────────────
    add("depth_prepass", std::make_unique<DepthPrePass>());
    add("hiz", std::make_unique<HizPass>());

    // ── Mesh shader passes (conditional) ───────────────────────────────
    if (gpu.use_mesh_shaders()) {
        if (!create_meshlet_passes(passes, gpu, resources))
            throw std::runtime_error("Failed to create meshlet passes");
    }

    // ── Shadow ─────────────────────────────────────────────────────────
    add("shadow", std::make_unique<ShadowPass>());

    // ── Debug / visualization passes ───────────────────────────────────
    add("depth_viz", std::make_unique<DepthVizPass>());
    add("debug", std::make_unique<DebugPass>());
    add("grid", std::make_unique<GridPass>());
    add("wireframe", std::make_unique<WireframePass>());
    add("normals", std::make_unique<NormalPass>());
    add("overdraw", std::make_unique<OverdrawPass>());
    add("uv_viz", std::make_unique<UvVizPass>());

    // ── Post-processing ────────────────────────────────────────────────
    add("bloom", std::make_unique<BloomPass>());

    // ── Sky ────────────────────────────────────────────────────────────
    add("sky", std::make_unique<SkyPass>());
    add("sky_cubemap", std::make_unique<SkyCubemapPass>());

    // ── Color passes ───────────────────────────────────────────────────
    add("transparent", std::make_unique<TransparentPass>());
    add("opaque", std::make_unique<OpaquePass>());
    add("screen_color_copy", std::make_unique<ScreenColorCopyPass>());
    add("distortion", std::make_unique<DistortionPass>());
    add("subsystem_compute", std::make_unique<SubsystemComputePass>());
    add("tonemap", std::make_unique<TonemapPass>());

    // ── Screen-space effects ───────────────────────────────────────────
    add("gtao", std::make_unique<GtaoPass>());
    add("ssr", std::make_unique<SsrPass>());
    add("volumetric_fog", std::make_unique<VolumetricFogPass>());
    add("ddgi", std::make_unique<DdgiPass>());

    // ── Forward+ lighting ──────────────────────────────────────────────
    add("light_cull", std::make_unique<LightCullPass>());
    {
        auto& hm = passes.add<LightVizPass>("light_heatmap", std::make_unique<LightVizPass>());
        hm.set_mode(0);
        auto r = hm.create(device, resources);
        if (!r) throw std::runtime_error(r.error());
    }
    {
        auto& lg = passes.add<LightVizPass>("light_grid", std::make_unique<LightVizPass>());
        lg.set_mode(1);
        auto r = lg.create(device, resources);
        if (!r) throw std::runtime_error(r.error());
    }

    // ── Scene SDF ──────────────────────────────────────────────────────
    add("scene_sdf", std::make_unique<SceneSdfPass>());

    // ── Post-creation hooks ────────────────────────────────────────────
    // Runs post_create() on each pass in insertion order.
    // Shadow, SkyCubemap, and SceneSdf write their global descriptors here.
    // SkyCubemap also generates the initial cubemap (one-shot GPU submit).
    passes.post_create_all(gpu);
}

bool create_meshlet_passes(PassRegistry& passes, GpuContext& gpu,
                           const PassResources& resources) {
    auto& device = gpu.device_ref();

    auto add = [&](const std::string& name, auto pass) -> bool {
        using T = std::remove_reference_t<decltype(*pass)>;
        auto& ref = passes.add<T>(name, std::move(pass));
        return static_cast<bool>(ref.create(device, resources));
    };

    if (!add("meshlet_instance", std::make_unique<MeshletInstancePass>()))
        return false;

    // Cull pass 2: retests rejected meshlets against freshly-built Hi-Z
    auto pass2 = std::make_unique<MeshletCullPass2>();
    pass2->set_instance_pass(&passes.get<MeshletInstancePass>("meshlet_instance"));
    if (!add("meshlet_cull_pass2", std::move(pass2)))
        return false;

    return add("mesh_shader_depth", std::make_unique<MeshShaderDepthPass>())
        && add("meshlet_viz", std::make_unique<MeshletVizPass>())
        && add("cull", std::make_unique<CullPass>())
        && add("cull_viz", std::make_unique<CullVizPass>());
}

} // namespace fjell
