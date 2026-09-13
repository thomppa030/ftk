#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <string>

namespace fjell {

class BindlessTextures;
class PipelineCache;

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

    [[nodiscard]] float atmosphere_rayleigh_scale() const { return atm_rayleigh_scale_; }
    [[nodiscard]] float atmosphere_mie_scale() const { return atm_mie_scale_; }
    [[nodiscard]] float atmosphere_mie_g() const { return atm_mie_g_; }
    [[nodiscard]] float atmosphere_mie_coeff() const { return atm_mie_coeff_; }
    [[nodiscard]] float atmosphere_sun_intensity() const { return atm_sun_intensity_; }
    [[nodiscard]] float atmosphere_hdri_floor() const { return atm_hdri_floor_; }

private:
    /// Open the sky's rendering scope: color + metallic-roughness + normal
    /// loaded, depth bound read-only. Both the built-in and FJSL paths
    /// render into it.
    void begin_rendering(const FrameContext& ctx);

    // Build the FJSL pipeline + allocate its material/params row. Called
    // when sky_shader_ changes. Idempotent: safe to call on the same path
    // multiple times — only rebuilds when the path changes or the FJSL is
    // hot-reloaded.
    void ensure_fjsl_pipeline(const PassResources& resources);
    void destroy_fjsl_pipeline();

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    glm::vec3 top_color_{0.15f, 0.25f, 0.45f};
    glm::vec3 bottom_color_{0.8f, 0.4f, 0.1f};
    Mode mode_{Mode::Gradient};

    // FJSL sky state. Empty path = built-in sky path is active.
    std::string sky_shader_;          // canonical path, e.g. "shaders/gradient_sky.fjsl"
    std::string fjsl_pipeline_shader_;// last path we built the pipeline for
    uint32_t fjsl_pipeline_id_{0};
    uint32_t fjsl_material_id_{0};
    VkPipelineLayout fjsl_layout_{VK_NULL_HANDLE};
    VkPipeline fjsl_pipeline_{VK_NULL_HANDLE};
    PipelineCache* fjsl_pipeline_cache_{nullptr};
    BindlessTextures* fjsl_bindless_{nullptr};
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
