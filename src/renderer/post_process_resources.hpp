#pragma once

#include "renderer/pass/bloom_pass.hpp"
#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/pass/gtao_pass.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/pass/rt_reflection_pass.hpp"
#include "renderer/pass/ss_gi_pass.hpp"
#include "renderer/pass/ssr_pass.hpp"
#include "renderer/pass/taa_resolve_pass.hpp"
#include "renderer/pass/post_pass.hpp"
#include "renderer/pass/volumetric_fog_pass.hpp"
#include "renderer/view_history.hpp"


#include <cstdint>
#include <memory>

namespace fjell {

/// Per-feature toggle for post-processing passes.
struct PostProcessConfig {
    bool hiz{true};
    bool gtao{true};
    bool bloom{true};
    bool ssr{false};
    bool volumetric_fog{false};
    bool post{true};
    bool ddgi{false};
    bool ss_gi{false};
    bool taa{false};
    bool rt_reflection{false};
};

struct FjGraph;

/// Derive a `PostProcessConfig` from a `.fjgraph`. Each config field is
/// true iff the corresponding pass class is present in the graph and
/// `enabled: true`. This is the load-bearing mapping that lets a viewport
/// hand a single `.fjgraph` filename and have everything (which passes
/// run + which per-viewport images to allocate) follow from one source.
[[nodiscard]] PostProcessConfig pp_config_from_graph(const FjGraph& graph);

/// Owns all per-viewport post-processing image state.
/// One instance per viewport / SubViewport. Pass singletons provide the
/// shared pipelines; this class owns the per-resolution images.
///
/// Bloom and SSR transients live in the FrameGraph's TransientImagePool —
/// no per-viewport Images for them. Config toggles still gate whether
/// those passes run (see `config()`).
class PostProcessResources {
public:
    /// The passes whose settings size images made here: DDGI's probe grid
    /// and SS-GI's probe spacing.
    struct PassRefs {
        DdgiPass& ddgi;
        SsGiPass& ss_gi;
    };

    /// `gpu` is the GPU interface's device, which makes the images.
    PostProcessResources(ftk::gpu::Device& gpu, uint32_t width, uint32_t height,
                         const PostProcessConfig& config, const PassRefs& passes);
    ~PostProcessResources();

    PostProcessResources(const PostProcessResources&) = delete;
    PostProcessResources& operator=(const PostProcessResources&) = delete;

    void resize(uint32_t width, uint32_t height);

    // Per-pass image access (nullptr if disabled in config)
    [[nodiscard]] HizPass::Images* hiz() { return config_.hiz ? &hiz_images_ : nullptr; }
    [[nodiscard]] AoPass::Images* gtao() { return config_.gtao ? &gtao_images_ : nullptr; }
    [[nodiscard]] VolumetricFogPass::Images* volumetric_fog() { return config_.volumetric_fog ? &fog_images_ : nullptr; }
    [[nodiscard]] PostPass::Images* post() { return config_.post ? &post_images_ : nullptr; }
    [[nodiscard]] DdgiPass::Images* ddgi() { return config_.ddgi ? &ddgi_images_ : nullptr; }
    [[nodiscard]] SsGiPass::Images* ss_gi() { return config_.ss_gi ? &ss_gi_images_ : nullptr; }
    [[nodiscard]] TaaResolvePass::Images* taa() { return config_.taa ? &taa_images_ : nullptr; }
    [[nodiscard]] RtReflectionPass::Images* rt_reflection() { return config_.rt_reflection ? &rt_reflection_images_ : nullptr; }

    [[nodiscard]] const PostProcessConfig& config() const { return config_; }

    /// Camera this viewport rendered with last frame; the temporal passes
    /// above reproject through it.
    [[nodiscard]] ViewHistory& view_history() { return view_history_; }

private:
    ftk::gpu::Device* gpu_{nullptr};
    PostProcessConfig config_;
    PassRefs passes_;

    HizPass::Images hiz_images_;
    AoPass::Images gtao_images_;
    VolumetricFogPass::Images fog_images_;
    PostPass::Images post_images_;
    DdgiPass::Images ddgi_images_;
    SsGiPass::Images ss_gi_images_;
    TaaResolvePass::Images taa_images_;
    RtReflectionPass::Images rt_reflection_images_;
    ViewHistory view_history_;
};

} // namespace fjell
