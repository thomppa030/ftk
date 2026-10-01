#pragma once

#include "environment/environment_state.hpp"
#include "environment/environment_ubo.hpp"
#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

class SkyPass;

/// The lighting environment surfaces sample through the global set: the sky
/// as a cubemap, mipmapped for rough reflections, and the split-sum BRDF
/// lookup table (512x512 RG16F) for IBL. The cubemap is the procedural sky
/// (gradient, atmosphere or FJSL) rendered at 64x64 a face when it changes,
/// or an HDRI converted and prefiltered at 512x512 a face when one is set,
/// which replaces it. Everything is generated in the frame, in this pass.
class SkyCubemapPass : public RenderPass {
public:
    ftk::Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(ftk::PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void register_resources(ftk::ResourceRegistry& registry) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    /// Whether an HDRI replaces the procedural cubemap.
    [[nodiscard]] bool has_hdri() const { return static_cast<bool>(hdri_cubemap_); }

    /// The cubemap the surfaces sample now: the HDRI's when there is one.
    [[nodiscard]] ftk::gpu::Texture sky_texture() const {
        return hdri_cubemap_ ? hdri_cubemap_.get() : cubemap_.get();
    }
    /// The sampler the surfaces read it with.
    [[nodiscard]] ftk::gpu::Sampler sky_sampler() const { return cubemap_sampler_; }

    void set_project(const class Project* p) { project_ = p; }

private:
    /// Whether record() renders the procedural cubemap this frame. An HDRI
    /// replaces it entirely, so a stale gradient under one is not rendered
    /// until the HDRI goes away. declare() and record() both answer this so
    /// the graph sees exactly the frames that write.
    [[nodiscard]] bool generates_this_frame() const { return dirty_ && !has_hdri(); }

    /// Mark the cubemap stale when the gradient or the atmosphere changes.
    void set_colors(const glm::vec3& top, const glm::vec3& bottom);
    void set_atmosphere(bool enabled, const glm::vec3& sun_dir,
                        const glm::vec3& sun_color, float sun_intensity,
                        float rayleigh_scale, float mie_scale,
                        float mie_g, float mie_coeff);

    /// Points every frame slot's global set at sky_texture(); frames in
    /// flight keep the set they bound.
    void bind_sky(GpuContext& gpu);

    /// Reads an HDRI equirectangular image and makes what converting it
    /// takes, for record() to convert and prefilter in the frame. Keeps the
    /// current sky when the file cannot be read.
    void load_hdri(const std::string& path);

    // Each records its work into the frame; the graph has brought what it
    // writes to storage as declare() says.
    void generate_brdf_lut(ftk::gpu::CommandList& cmd);
    /// The built-in sky (gradient or atmosphere).
    void generate(ftk::gpu::CommandList& cmd);
    /// The active FJSL sky through its compute variant, so the sky that is
    /// drawn is the sky that lights; the built-in sky while it has none.
    void generate_fjsl(const FrameContext& ctx);
    /// Builds, or rebuilds after SkyPass compiled a new shader, the FJSL
    /// sky's compute variant.
    void ensure_fjsl_pipeline();
    /// Mip 0 of the procedural cubemap has been written as storage: fills
    /// the rest of the chain from it and leaves it all as a copy target.
    void finish_cubemap(ftk::gpu::CommandList& cmd);
    void convert_hdri(ftk::gpu::CommandList& cmd);

    static constexpr uint32_t CUBEMAP_SIZE = 64;
    static constexpr uint32_t MIP_LEVELS = 7; // floor(log2(64)) + 1
    static constexpr uint32_t HDRI_CUBEMAP_SIZE = 512;
    static constexpr uint32_t HDRI_MIP_LEVELS = 10; // floor(log2(512)) + 1
    static constexpr uint32_t BRDF_LUT_SIZE = 512;

    ftk::gpu::Device* device_{nullptr};
    PassResources resources_{};

    ftk::gpu::Owned<ftk::gpu::Texture> cubemap_;
    ftk::gpu::Owned<ftk::gpu::Texture> brdf_lut_;
    /// Trilinear, for the cubemaps' mips; linear, for the lookup table.
    ftk::gpu::Sampler cubemap_sampler_;
    ftk::gpu::Sampler brdf_lut_sampler_;
    ftk::gpu::Owned<ftk::gpu::ComputePipeline> cubemap_pipeline_;
    ftk::gpu::Owned<ftk::gpu::ComputePipeline> brdf_pipeline_;
    ftk::gpu::Owned<ftk::gpu::ComputePipeline> equirect_pipeline_;
    ftk::gpu::Owned<ftk::gpu::ComputePipeline> prefilter_pipeline_;
    bool brdf_generated_{false};

    glm::vec3 top_color_{0.15f, 0.25f, 0.45f};
    glm::vec3 bottom_color_{0.35f, 0.35f, 0.4f};

    bool atmosphere_enabled_{false};
    glm::vec3 atm_sun_dir_{0.0f, 1.0f, 0.0f};
    glm::vec3 atm_sun_color_{1.0f};
    float atm_sun_intensity_{22.0f};
    float atm_rayleigh_scale_{8500.0f};
    float atm_mie_scale_{1200.0f};
    float atm_mie_g_{0.76f};
    float atm_mie_coeff_{21e-6f};

    bool dirty_{true};

    // FJSL sky: the sky shader's compute variant over the six faces. Binds
    // the global set, the bindless set and the faces as storage.
    SkyPass* sky_pass_{nullptr};
    bool fjsl_active_{false};
    ftk::gpu::Owned<ftk::gpu::ComputePipeline> fjsl_pipeline_;
    uint32_t fjsl_pipeline_id_{0};
    uint32_t fjsl_generation_{0};
    // What the cubemap last rendered; a change re-renders it.
    struct FjslInputs {
        EnvironmentUbo ubo{};
        std::vector<NamedValue> uniforms;
        uint32_t generation{0};
        bool operator==(const FjslInputs&) const = default;
    };
    FjslInputs fjsl_inputs_{};
    bool fjsl_rendered_{false};
    ftk::ResourceRegistry* registry_{nullptr};
    const class Project* project_{nullptr};

    // ── HDRI environment map ──────────────────────────────────────────
    std::string loaded_hdri_path_;
    /// The prefiltered cubemap surfaces sample: roughness rises with the mip.
    ftk::gpu::Owned<ftk::gpu::Texture> hdri_cubemap_;
    /// Until the frame that converts them: the image as uploaded, and the
    /// same as a cube with a plain mip chain, which the prefilter samples.
    ftk::gpu::Owned<ftk::gpu::Texture> hdri_equirect_;
    ftk::gpu::Owned<ftk::gpu::Texture> hdri_source_;
    bool hdri_pending_{false};
};

} // namespace fjell
