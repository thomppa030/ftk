#pragma once

#include "core/named_value.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/post_layers.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fjell {

class BindlessTextures;
class PipelineCache;

// The post pass: what the viewport shows, drawn from the scene colour after
// TAA through `shader_type post` shaders. The look is the shaders', the tone
// curve included; the pass supplies the exposure, the bloom and the depth and
// writes the 8-bit sRGB image ImGui displays.
//
// Which shaders, with which values, is the scene's post process volumes as
// this viewport's camera stands among them: plan_post_layers() turns them into
// layers, and each layer is one fullscreen draw over the ones below. With no
// volume, the one layer is the engine's post.fjsl.
//
// A shader's values are per viewport, because each viewport's camera may sit
// somewhere different, and one frame renders several. So the pass does not
// write a material row's params: it writes each layer's values into the
// frame's transient memory, one slice per draw, and pushes where the slice
// starts. The material row supplies the textures.
class PostPass : public RenderPass {
public:
    /// The format of what the viewport shows, which the editor overlays draw
    /// into after this pass.
    static constexpr gpu::Format OUTPUT_FORMAT = gpu::Format::rgba8_srgb;

    /// The bindless index a sampler with this hint reads when nothing is
    /// bound: what the engine default's row holds in its texture slots, the
    /// neutral table for its grade among them.
    using TextureFallbacks = std::function<uint32_t(UniformHint)>;
    void set_texture_fallbacks(TextureFallbacks fallbacks) { texture_fallbacks_ = std::move(fallbacks); }

    /// Draw the engine default only, whatever volumes the camera is in: the
    /// frame as it is before any grade, to capture and grade elsewhere.
    /// Runtime state, never saved.
    void set_ungraded(bool on) { ungraded_ = on; }
    [[nodiscard]] bool ungraded() const { return ungraded_; }


    // Per-viewport output image.
    struct Images {
        /// What the viewport shows, in OUTPUT_FORMAT.
        gpu::Owned<gpu::Texture> texture;
        glm::uvec2 extent{0, 0};
        /// Whether the pass has drawn into the image since it was made.
        bool written{false};

        void create(gpu::Device& gpu, uint32_t width, uint32_t height);
        /// Releases the output; the frames still reading it keep it until
        /// they are done.
        void destroy();
        void resize(gpu::Device& gpu, uint32_t width, uint32_t height);
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    void set_exposure(float ev) { exposure_ = ev; }
    [[nodiscard]] float exposure() const { return exposure_; }

    void set_auto_exposure(bool on) { auto_exposure_ = on; }
    [[nodiscard]] bool auto_exposure() const { return auto_exposure_; }

    void set_adaptation_speed(float s) { adaptation_speed_ = s; }
    [[nodiscard]] float adaptation_speed() const { return adaptation_speed_; }

    void set_exposure_range(float min_ev, float max_ev) { min_ev_ = min_ev; max_ev_ = max_ev; }

    /// The sampler the viewport images are shown with.
    [[nodiscard]] gpu::Sampler output_sampler() const { return linear_sampler_; }

private:
    /// A post shader's two pipelines. The first layer covers the image
    /// and draws opaque, as the one draw of the common case should; every
    /// layer after it blends over what is there by the weight it writes.
    struct MaterialPipeline {
        gpu::Owned<gpu::GraphicsPipeline> base;
        gpu::Owned<gpu::GraphicsPipeline> over;
        uint32_t generation{0};
    };
    /// The engine default, registered once and given a material row for its
    /// textures, and the layer it draws as: its own look at the .fjsl's
    /// values, rebuilt when the shader recompiles.
    struct Fallback {
        bool ready{false};
        uint32_t pipeline_id{0};
        uint32_t material_id{~0u};
        uint32_t generation{0};
        /// Whether the row's texture slots hold the fallbacks yet: they
        /// arrive after the pass is made.
        bool textured{false};
        PostLayer layer;
    };

    /// Build, or fetch, the pipeline for a post shader entry, kept current
    /// with the entry's compile generation so a hot reload reaches it. Null
    /// when the entry is not a post shader or has never compiled; a failed
    /// recompile keeps the last good one.
    const MaterialPipeline* ensure_material_pipeline(uint32_t pipeline_id);
    void ensure_default();
    void destroy_material_pipelines();

    /// What a volume draws with: its material, or the engine default when it
    /// has none or its shader has no pipeline to draw with.
    [[nodiscard]] PostLook look_for(const PostVolume& volume) const;
    /// A layer's values resolved against its shader's uniform block.
    [[nodiscard]] std::vector<float> layer_values(const PostLayer& layer) const;

    /// What the template's set 2 samples. `bloom` is unset when this
    /// viewport's graph builds no bloom.
    struct Inputs {
        gpu::Texture color{};
        gpu::Texture bloom{};
        gpu::Texture depth{};
        gpu::Texture normal{};
        gpu::Texture velocity{};
    };
    void record_into(const FrameContext& ctx, Images& imgs, const Inputs& inputs, float exposure,
                     std::span<const PostLayer> layers);

    gpu::Device* device_{nullptr};
    /// The engine's globals and bindless table, sets 0 and 1 of the template.
    gpu::SharedLayout globals_{};
    gpu::SharedLayout bindless_layout_{};

    PipelineCache* pipeline_cache_{nullptr};
    BindlessTextures* bindless_{nullptr};

    // The colours filter; the depth, the encoded normal and the motion are
    // read texel by texel.
    gpu::Sampler linear_sampler_;
    gpu::Sampler nearest_sampler_;

    std::unordered_map<uint32_t, MaterialPipeline> material_pipelines_;
    /// Entries already reported as unusable, so a broken shader is said once
    /// rather than every frame.
    std::unordered_set<uint32_t> reported_entries_;
    Fallback default_;
    TextureFallbacks texture_fallbacks_;
    bool ungraded_{false};

    float exposure_{0.0f};
    float luminance_{0.0f};
    bool auto_exposure_{false};
    float adaptation_speed_{1.5f};
    float min_ev_{-5.0f};
    float max_ev_{5.0f};
};

} // namespace fjell
