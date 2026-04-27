#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/render_settings.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <memory>

namespace fjell {

class AccelStructureManager;

// Ray-traced reflections. Slots into the same composite as SSR by writing
// rt_reflection_output (RGBA16F, viewport-sized). The composite picks
// between ssr_output and rt_reflection_output based on the active mode
// on SsrCore.
//
// Skinned meshes are not in the BLAS in V1 — they don't appear in
// reflections. Static meshes and particles do.
//
// Hardware-gated: the RT pipeline is only built when
// device.ray_tracing_supported(). When unsupported the pass still
// instantiates so .fjgraph parses, but record() short-circuits.
class RtReflectionPass : public RenderPass {
public:
    using Mode = RenderSettings::Ssr::Mode;

    /// Per-pass descriptor + UBO state. Currently single-instance (shared
    /// across viewports) — RT reflections aren't yet authored on a per-
    /// viewport basis. Promote into PostProcessResources when a project
    /// needs different reflection settings per camera.
    struct State {
        std::unique_ptr<Buffer> reflection_ubo;
        VkDescriptorPool desc_pool{VK_NULL_HANDLE};
        VkDescriptorSet trace_set{VK_NULL_HANDLE};
        bool first_use{true};
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    void set_accel_manager(AccelStructureManager* mgr) { accel_manager_ = mgr; }
    void set_mode(Mode m) { mode_ = m; }
    [[nodiscard]] Mode mode() const { return mode_; }
    [[nodiscard]] bool ray_traced_active() const {
        return mode_ == Mode::RayTraced && rt_pipeline_ != VK_NULL_HANDLE;
    }

    void set_sun_light(glm::vec3 direction, glm::vec3 color, float intensity) {
        sun_direction_ = direction;
        sun_color_ = color;
        sun_intensity_ = intensity;
    }

    void set_intensity(float i) { intensity_ = i; }
    [[nodiscard]] float intensity() const { return intensity_; }

private:
    Device* device_{nullptr};
    VkDevice vk_device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    AccelStructureManager* accel_manager_{nullptr};

    Mode mode_{Mode::ScreenSpace};
    float intensity_{1.0F};
    uint32_t frame_counter_{0};

    glm::vec3 sun_direction_{0.0F, -1.0F, 0.0F};
    glm::vec3 sun_color_{1.0F};
    float sun_intensity_{1.5F};

    // Samplers + RT pipeline. NULL when device lacks RT support.
    VkSampler linear_sampler_{VK_NULL_HANDLE};
    VkSampler nearest_sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout trace_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout rt_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline rt_pipeline_{VK_NULL_HANDLE};

    // Shader binding table
    std::unique_ptr<Buffer> sbt_buffer_;
    VkStridedDeviceAddressRegionKHR sbt_raygen_{};
    VkStridedDeviceAddressRegionKHR sbt_miss_{};
    VkStridedDeviceAddressRegionKHR sbt_hit_{};
    VkStridedDeviceAddressRegionKHR sbt_callable_{};

    State state_;
};

} // namespace fjell
