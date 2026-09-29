#pragma once

#include "environment/environment_state.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace fjell {

class BindlessTextures;
class PipelineCache;
struct ShaderMetadata;

/// The path SkyPass hands PipelineCache for a sky shader reference. A
/// project reference that leaves the engine tree is made absolute so the
/// cache does not read it as engine-relative; anything else passes through,
/// so a caller asking about the same reference gets the same compile.
[[nodiscard]] std::string canonical_sky_shader_path(const std::string& sky_shader,
                                                    const class Project* project);

// Draws a solid color or vertical gradient behind all geometry.
// Renders into the existing color pass (between begin/end rendering)
// using reverse depth test so it only fills pixels with no geometry.
class SkyPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void record(const FrameContext& ctx) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

    [[nodiscard]] const glm::vec3& top_color() const { return top_color_; }
    [[nodiscard]] const glm::vec3& bottom_color() const { return bottom_color_; }

    // Sky mode matches the shader's push.sky_mode:
    //   0 = gradient, 1 = HDRI cubemap, 2 = procedural atmospheric scattering,
    //   3 = FJSL custom (sky_shader_ is non-empty).
    enum class Mode : int { Gradient = 0, Hdri = 1, Atmosphere = 2, Fjsl = 3 };
    [[nodiscard]] Mode mode() const { return mode_; }

    // Wire dependencies needed for FJSL skies. SkyPass calls into the
    // pipeline cache (to compile the shader) and the bindless registry
    // (to allocate a material row + params slot) on demand.
    void set_pipeline_cache(PipelineCache* pc) { fjsl_pipeline_cache_ = pc; }
    void set_bindless(BindlessTextures* b) { fjsl_bindless_ = b; }
    // Optional project pointer. When set, FJSL sky paths are resolved
    // relative to the project root first (the inspector stores
    // project-relative paths). When null, paths are resolved engine-relative.
    void set_project(const class Project* p) { project_ = p; }

    // The compiled FJSL sky, for SkyCubemapPass to render the same shader
    // into the lighting cubemap: false / zero while no FJSL sky is active.
    [[nodiscard]] bool fjsl_active() const { return mode_ == Mode::Fjsl && static_cast<bool>(fjsl_pipeline_); }
    [[nodiscard]] uint32_t fjsl_pipeline_id() const { return fjsl_pipeline_id_; }
    [[nodiscard]] uint32_t fjsl_material_id() const { return fjsl_material_id_; }
    [[nodiscard]] uint32_t fjsl_pipeline_generation() const { return fjsl_pipeline_generation_; }
    [[nodiscard]] PipelineCache* pipeline_cache() const { return fjsl_pipeline_cache_; }

    [[nodiscard]] float atmosphere_rayleigh_scale() const { return atm_rayleigh_scale_; }
    [[nodiscard]] float atmosphere_mie_scale() const { return atm_mie_scale_; }
    [[nodiscard]] float atmosphere_mie_g() const { return atm_mie_g_; }
    [[nodiscard]] float atmosphere_mie_coeff() const { return atm_mie_coeff_; }
    [[nodiscard]] float atmosphere_sun_intensity() const { return atm_sun_intensity_; }
    [[nodiscard]] float atmosphere_hdri_floor() const { return atm_hdri_floor_; }

private:
    // Build the FJSL pipeline + allocate its material/params row. Called
    // when sky_shader_ changes. Idempotent: safe to call on the same path
    // multiple times — only rebuilds when the path changes or the FJSL is
    // hot-reloaded.
    void ensure_fjsl_pipeline(const PassResources& resources);
    // Write the shader's default uniform values into its params slice.
    void write_default_params(const ShaderMetadata& meta, uint32_t param_offset);
    // Lay the environment's named values over the defaults, once per change.
    void apply_uniform_values();

    /// The built-in skies: gradient, HDRI and atmosphere.
    gpu::Owned<gpu::GraphicsPipeline> pipeline_;

    glm::vec3 top_color_{0.15f, 0.25f, 0.45f};
    glm::vec3 bottom_color_{0.8f, 0.4f, 0.1f};
    Mode mode_{Mode::Gradient};

    // FJSL sky state. Empty path = built-in sky path is active.
    std::string sky_shader_;          // canonical path, e.g. "shaders/gradient_sky.fjsl"
    std::string fjsl_pipeline_shader_;// last path we built the pipeline for
    uint32_t fjsl_pipeline_id_{0};
    uint32_t fjsl_pipeline_generation_{0}; // PipelineCache generation the pipeline was built at
    /// The sky's row in the material buffer, and the params slice it points
    /// at. Both are kept across a rebuild: the row is the sky's identity, not
    /// the pipeline's, and taking a fresh one every recompile leaked one per
    /// shader reload with nothing to give them back. ~0u is "never taken",
    /// since zero is a real row.
    uint32_t fjsl_material_id_{~0u};
    uint32_t fjsl_param_offset_{0};
    uint32_t fjsl_param_count_{0};
    /// The FJSL sky's fragment on the built-in sky's triangle; unset while
    /// no FJSL sky is built.
    gpu::Owned<gpu::GraphicsPipeline> fjsl_pipeline_;
    PipelineCache* fjsl_pipeline_cache_{nullptr};
    BindlessTextures* fjsl_bindless_{nullptr};
    // The environment's sky uniform values this frame, and the set the params
    // slice currently holds; a rebuild clears the latter so they reapply.
    std::vector<NamedValue> uniform_values_;
    std::vector<NamedValue> applied_uniform_values_;
    bool uniform_values_applied_{false};
    const class Project* project_{nullptr};

    // PassResources kept across frames so prepare_frame can rebuild the
    // FJSL pipeline when sky_shader_ changes.
    PassResources cached_resources_{};

    // Cached atmosphere parameters from the scene's WorldEnvironmentNode.
    float atm_rayleigh_scale_{8500.0f};
    float atm_mie_scale_{1200.0f};
    float atm_mie_g_{0.76f};
    float atm_mie_coeff_{21e-6f};
    float atm_sun_intensity_{22.0f};
    float atm_hdri_floor_{0.1f};
    bool  has_hdri_{false};
};

} // namespace fjell
