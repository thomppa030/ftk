// Auto-registers every render pass with `PassFactory` so `.fjgraph` can
// instantiate them by class name. Registrations live here (one place to
// audit) instead of being sprinkled across each pass `.cpp`.
//
// Adding a new pass:
//   1. include its header below
//   2. call FJELL_REGISTER_PASS(MyPass) — or FJELL_REGISTER_PASS_FN if it
//      takes constructor params
//   3. add the class name to a `.fjgraph` file (or the default one) to
//      bring it into a pipeline

#include "renderer/pass_factory.hpp"

#include "renderer/pass/bloom_pass.hpp"
#include "renderer/pass/cloud_shadow_pass.hpp"
#include "renderer/pass/fog_density_field_pass.hpp"
#include "renderer/pass/water_fft_pass.hpp"
#include "renderer/pass/motion_field_pass.hpp"
#include "renderer/pass/contact_shadow_pass.hpp"
#include "renderer/pass/cull_pass.hpp"
#include "renderer/pass/cull_viz_pass.hpp"
#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/pass/debug_pass.hpp"
#include "renderer/pass/depth_prepass.hpp"
#include "renderer/pass/depth_resolve_pass.hpp"
#include "renderer/pass/depth_viz_pass.hpp"
#include "renderer/pass/distortion_apply_pass.hpp"
#include "renderer/pass/distortion_pass.hpp"
#include "renderer/pass/exposure_pass.hpp"
#include "renderer/pass/grid_pass.hpp"
#include "renderer/pass/gtao_pass.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/pass/instance_cull_pass.hpp"
#include "renderer/pass/light_cull_pass.hpp"
#include "renderer/pass/light_viz_pass.hpp"
#include "renderer/pass/local_shadow_pass.hpp"
#include "renderer/pass/terrain_pass.hpp"
#include "renderer/pass/water_pass.hpp"
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
#include "renderer/pass/rt_reflection_pass.hpp"
#include "renderer/pass/ssr_pass.hpp"
#include "renderer/pass/ss_gi_pass.hpp"
#include "renderer/pass/subsystem_compute_pass.hpp"
#include "renderer/pass/sun_shafts_pass.hpp"
#include "renderer/pass/taa_resolve_pass.hpp"
#include "renderer/pass/tonemap_pass.hpp"
#include "renderer/pass/transparent_pass.hpp"
#include "renderer/pass/uv_viz_pass.hpp"
#include "renderer/pass/volumetric_fog_pass.hpp"
#include "renderer/pass/wireframe_pass.hpp"

namespace fjell {

// ── Default-constructible passes ──────────────────────────────────────
FJELL_REGISTER_PASS(BloomDs0Pass)
FJELL_REGISTER_PASS(ContactShadowPass)
FJELL_REGISTER_PASS(CloudShadowPass)
FJELL_REGISTER_PASS(MotionFieldPass)
FJELL_REGISTER_PASS(FogDensityFieldPass)
FJELL_REGISTER_PASS(WaterFftPass)
FJELL_REGISTER_PASS(CullPass)
FJELL_REGISTER_PASS(CullVizPass)
FJELL_REGISTER_PASS(DdgiPass)
FJELL_REGISTER_PASS(DebugPass)
FJELL_REGISTER_PASS(DepthPrePass)
FJELL_REGISTER_PASS(DepthResolvePass)
FJELL_REGISTER_PASS(DepthVizPass)
FJELL_REGISTER_PASS(DistortionApplyPass)
FJELL_REGISTER_PASS(DistortionPass)
FJELL_REGISTER_PASS(ExposurePass)
FJELL_REGISTER_PASS(FogCompositePass)
FJELL_REGISTER_PASS(FogMediumPass)
FJELL_REGISTER_PASS(FogLightPass)
FJELL_REGISTER_PASS(FogIntegratePass)
FJELL_REGISTER_PASS(FogTemporalPass)
FJELL_REGISTER_PASS(GridPass)
FJELL_REGISTER_PASS(GtaoTracePass)
FJELL_REGISTER_PASS(AoBlurHPass)
FJELL_REGISTER_PASS(AoBlurVPass)
FJELL_REGISTER_PASS(HizPass)
FJELL_REGISTER_PASS(InstanceCullPass)
FJELL_REGISTER_PASS(LightCullPass)
FJELL_REGISTER_PASS(LocalShadowPass)
FJELL_REGISTER_PASS(TerrainPass)
FJELL_REGISTER_PASS(WaterPass)
FJELL_REGISTER_PASS(MeshShaderDepthPass)
FJELL_REGISTER_PASS(MeshletCullPass2)
FJELL_REGISTER_PASS(MeshletInstancePass)
FJELL_REGISTER_PASS(MeshletVizPass)
FJELL_REGISTER_PASS(NormalPass)
FJELL_REGISTER_PASS(OpaquePass)
FJELL_REGISTER_PASS(OverdrawPass)
FJELL_REGISTER_PASS(SceneSdfPass)
FJELL_REGISTER_PASS(ScreenColorCopyPass)
FJELL_REGISTER_PASS(ShadowPass)
FJELL_REGISTER_PASS(SkyCubemapPass)
FJELL_REGISTER_PASS(SkyPass)
FJELL_REGISTER_PASS(RtReflectionPass)
FJELL_REGISTER_PASS(SsrCompositePass)
FJELL_REGISTER_PASS(SsrHistoryPass)
FJELL_REGISTER_PASS(SsrTracePass)
FJELL_REGISTER_PASS(SsGiPass)
FJELL_REGISTER_PASS(SsGiDepthHistoryPass)
FJELL_REGISTER_PASS(SubsystemComputePass)
FJELL_REGISTER_PASS(SunShaftsCompositePass)
FJELL_REGISTER_PASS(SunShaftsPass)
FJELL_REGISTER_PASS(SunShaftsResolvePass)
FJELL_REGISTER_PASS(TaaResolvePass)
FJELL_REGISTER_PASS(TonemapPass)
FJELL_REGISTER_PASS(TransparentPass)
FJELL_REGISTER_PASS(UvVizPass)
FJELL_REGISTER_PASS(WireframePass)

// ── Parameterized passes ──────────────────────────────────────────────
//
// `.fjgraph` writes these as e.g.
//   { "class": "BloomDsPass", "params": { "mip_index": 1 } }
// The factory closure pulls keys it cares about; everything else is
// per-pass setter territory (handled at register-with-pipeline time).

FJELL_REGISTER_PASS_FN(BloomDsPass, [](const PassParams& p) {
    return std::make_unique<BloomDsPass>(
        static_cast<uint32_t>(p.int_or("mip_index", 1)));
})

FJELL_REGISTER_PASS_FN(BloomUsPass, [](const PassParams& p) {
    return std::make_unique<BloomUsPass>(
        static_cast<uint32_t>(p.int_or("dst_mip", 0)));
})

FJELL_REGISTER_PASS_FN(LightVizPass, [](const PassParams& p) {
    auto pass = std::make_unique<LightVizPass>();
    pass->set_mode(static_cast<uint32_t>(p.int_or("mode", 0)));
    return pass;
})

} // namespace fjell
