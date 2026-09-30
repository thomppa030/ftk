#include "renderer/post_process_resources.hpp"

#include "renderer/fjgraph_asset.hpp"

#include <algorithm>
#include <unordered_map>

namespace fjell {

PostProcessConfig pp_config_from_graph(const FjGraph& graph) {
    // Fixed pass-class → config-field map. New post-processing passes
    // need an entry here so per-viewport graphs can drive their
    // image allocation.
    static const std::unordered_map<std::string, void(*)(PostProcessConfig&, bool)> kSetters{
        {"HizPass",        [](PostProcessConfig& c, bool v) { c.hiz = v; }},
        {"GtaoTracePass",  [](PostProcessConfig& c, bool v) { c.gtao = v; }},
        {"BloomDs0Pass",   [](PostProcessConfig& c, bool v) { c.bloom = v; }},
        {"SsrTracePass",   [](PostProcessConfig& c, bool v) { c.ssr = v; }},
        {"FogMediumPass",  [](PostProcessConfig& c, bool v) { c.volumetric_fog = v; }},
        {"PostPass",       [](PostProcessConfig& c, bool v) { c.post = v; }},
        {"DdgiPass",       [](PostProcessConfig& c, bool v) { c.ddgi = v; }},
        {"SsGiPass",       [](PostProcessConfig& c, bool v) { c.ss_gi = v; }},
        {"TaaResolvePass", [](PostProcessConfig& c, bool v) { c.taa = v; }},
        {"RtReflectionPass", [](PostProcessConfig& c, bool v) { c.rt_reflection = v; }},
    };

    // Start with everything off; flip on each pass that the graph names.
    // Any feature not present in the graph stays off — no per-viewport
    // images allocated for it.
    PostProcessConfig cfg{};
    cfg.hiz = false;
    cfg.gtao = false;
    cfg.bloom = false;
    cfg.ssr = false;
    cfg.volumetric_fog = false;
    cfg.post = false;
    cfg.ddgi = false;
    cfg.ss_gi = false;
    cfg.taa = false;
    cfg.rt_reflection = false;

    for (const auto& entry : graph.passes) {
        auto it = kSetters.find(entry.class_name);
        if (it != kSetters.end()) { it->second(cfg, entry.enabled); }
    }
    return cfg;
}

PostProcessResources::PostProcessResources(gpu::Device& gpu, uint32_t width, uint32_t height,
                                            const PostProcessConfig& config, const PassRefs& passes)
    : gpu_(&gpu), config_(config), passes_(passes) {
    if (config_.hiz && width > 0 && height > 0) {
        hiz_images_.create(*gpu_,
                           width, height);
    }
    if (config_.gtao) {
        uint32_t half_w = std::max(1u, width / 2);
        uint32_t half_h = std::max(1u, height / 2);
        gtao_images_.create(*gpu_, half_w, half_h);
    }
    if (config_.volumetric_fog) {
        fog_images_.create(*gpu_, width, height);
    }
    if (config_.post && width > 0 && height > 0) {
        post_images_.create(*gpu_, width, height);
    }
    if (config_.taa && width > 0 && height > 0) {
        taa_images_.create(*gpu_, width, height);
    }
    if (config_.rt_reflection && width > 0 && height > 0) {
        rt_reflection_images_.create(*gpu_, width, height);
    }
    if (config_.ddgi && width > 0 && height > 0) {
        ddgi_images_.resize(*gpu_, passes_.ddgi.config(), width, height);
    }
    if (config_.ss_gi && width > 0 && height > 0) {
        ss_gi_images_.resize(*gpu_, passes_.ss_gi.config(), width, height);
    }
}

PostProcessResources::~PostProcessResources() {
    hiz_images_.destroy();
    gtao_images_.destroy();
    fog_images_.destroy();
    post_images_.destroy();
    ddgi_images_.destroy();
    ss_gi_images_.destroy();
    taa_images_.destroy();
    rt_reflection_images_.destroy();
}

void PostProcessResources::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;

    if (config_.hiz) {
        hiz_images_.resize(*gpu_,
                           width, height);
    }
    if (config_.gtao) {
        gtao_images_.resize(*gpu_, width, height);
    }
    if (config_.volumetric_fog) {
        fog_images_.resize(*gpu_, width, height);
    }
    if (config_.post) {
        post_images_.resize(*gpu_, width, height);
    }
    if (config_.taa) {
        taa_images_.resize(*gpu_, width, height);
    }
    if (config_.rt_reflection) {
        rt_reflection_images_.resize(*gpu_, width, height);
    }
    if (config_.ddgi) {
        ddgi_images_.resize(*gpu_, passes_.ddgi.config(), width, height);
    } else if (ddgi_images_.radiance) {
        ddgi_images_.destroy();
    }
    if (config_.ss_gi) {
        ss_gi_images_.resize(*gpu_, passes_.ss_gi.config(), width, height);
    } else if (ss_gi_images_.indirect) {
        ss_gi_images_.destroy();
    }
}

} // namespace fjell
