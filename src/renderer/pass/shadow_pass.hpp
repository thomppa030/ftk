#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"
#include "renderer/pass/depth_variants.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace fjell {


class ShadowPass : public RenderPass {
public:
    static constexpr uint32_t CASCADE_COUNT = FrameContext::MAX_SHADOW_CASCADES;
    static constexpr uint32_t MAP_SIZE = 4096;
    static constexpr gpu::Format DEPTH_FORMAT = gpu::Format::d32_float;
    /// Slope-scaled depth bias every caster is rasterized with. Shared so a
    /// pass that draws into a cascade with its own pipeline — terrain does —
    /// biases the same way; a cascade written half at one bias reads as bands
    /// across the ground.
    static constexpr float DEPTH_BIAS_CONSTANT = 1.25F;
    static constexpr float DEPTH_BIAS_SLOPE = 1.75F;
    /// Every caster's front faces are culled, whatever its material culls:
    /// with the bias, it keeps acne off lit surfaces.
    static constexpr gpu::Cull CASTER_CULL = gpu::Cull::front;

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    /// Create a custom shadow pipeline for a specific shader pipeline ID
    /// using a custom vertex shader SPV.
    void register_custom_shadow(uint32_t pipeline_id, const std::string& vert_spv_path) {
        casters_.register_custom(pipeline_id, vert_spv_path, CASTER_CULL);
    }
    /// Forget every custom shadow pipeline.
    void clear_custom_shadows() { casters_.clear_custom(); }

    [[nodiscard]] const std::array<glm::mat4, CASCADE_COUNT>& light_matrices() const {
        return light_matrices_;
    }
    [[nodiscard]] const std::array<float, CASCADE_COUNT>& cascade_splits() const {
        return cascade_splits_;
    }

private:
    // Compute cascade splits and light-space matrices for this frame.
    void update_cascades(const glm::mat4& view, const glm::mat4& projection,
                         const glm::vec3& light_direction, float near, float far);

    gpu::Device* device_{nullptr};

    /// The cascades, one layer each.
    gpu::Owned<gpu::Texture> atlas_;
    /// Whether the pass has drawn into the cascades since they were made.
    bool written_{false};
    /// Compares against the cascades for filtered shadow lookups.
    gpu::Sampler sampler_;

    /// The batched casters' pipelines, custom vertex stages included.
    DepthVariants casters_;
    gpu::Owned<gpu::GraphicsPipeline> skinned_pipeline_;

    // Per-frame cascade data
    std::array<glm::mat4, CASCADE_COUNT> light_matrices_{};
    std::array<float, CASCADE_COUNT> cascade_splits_{};
};

} // namespace fjell
