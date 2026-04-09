#pragma once

#include "renderer/pass/bloom_pass.hpp"
#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/pass/gtao_pass.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/pass/ssr_pass.hpp"
#include "renderer/pass/tonemap_pass.hpp"
#include "renderer/pass/volumetric_fog_pass.hpp"

#include <vulkan/vulkan.h>

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
    bool tonemap{true};
    bool ddgi{false};
};

/// Owns all per-viewport post-processing image state.
/// One instance per viewport / SubViewport. Pass singletons provide the
/// shared pipelines and descriptor set layouts; this class owns the
/// per-resolution images, descriptor pools, and descriptor sets.
class PostProcessResources {
public:
    /// Pass singletons are needed to provide set layouts for Images creation.
    struct PassRefs {
        HizPass& hiz;
        GtaoPass& gtao;
        BloomPass& bloom;
        SsrPass& ssr;
        VolumetricFogPass& fog;
        TonemapPass& tonemap;
        DdgiPass& ddgi;
    };

    PostProcessResources(VkDevice device, VmaAllocator allocator,
                          uint32_t width, uint32_t height,
                          const PostProcessConfig& config,
                          const PassRefs& passes);
    ~PostProcessResources();

    PostProcessResources(const PostProcessResources&) = delete;
    PostProcessResources& operator=(const PostProcessResources&) = delete;
    PostProcessResources(PostProcessResources&& other) noexcept;
    PostProcessResources& operator=(PostProcessResources&& other) noexcept;

    void resize(uint32_t width, uint32_t height);

    // Per-pass image access (nullptr if disabled in config)
    [[nodiscard]] HizPass::Images* hiz() { return config_.hiz ? &hiz_images_ : nullptr; }
    [[nodiscard]] GtaoPass::Images* gtao() { return config_.gtao ? &gtao_images_ : nullptr; }
    [[nodiscard]] BloomPass::Images* bloom() { return config_.bloom ? &bloom_images_ : nullptr; }
    [[nodiscard]] SsrPass::Images* ssr() { return config_.ssr ? &ssr_images_ : nullptr; }
    [[nodiscard]] VolumetricFogPass::Images* volumetric_fog() { return config_.volumetric_fog ? &fog_images_ : nullptr; }
    [[nodiscard]] TonemapPass::Images* tonemap() { return config_.tonemap ? &tonemap_images_ : nullptr; }
    [[nodiscard]] DdgiPass::Images* ddgi() { return config_.ddgi ? &ddgi_images_ : nullptr; }

    [[nodiscard]] const PostProcessConfig& config() const { return config_; }

    /// Release all imgui texture descriptors (call before swapchain recreation).
    void invalidate_imgui_textures();

private:
    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    PostProcessConfig config_;

    // Cached layout/sampler refs for resize (non-owning)
    std::unique_ptr<PassRefs> passes_storage_;
    PassRefs* passes_{nullptr};

    HizPass::Images hiz_images_;
    GtaoPass::Images gtao_images_;
    BloomPass::Images bloom_images_;
    SsrPass::Images ssr_images_;
    VolumetricFogPass::Images fog_images_;
    TonemapPass::Images tonemap_images_;
    DdgiPass::Images ddgi_images_;
};

} // namespace fjell
