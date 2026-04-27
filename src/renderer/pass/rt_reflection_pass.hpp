#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/render_settings.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
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
// Hardware-gated: pipeline objects only created when
// device.ray_tracing_supported(). When unsupported the pass still
// instantiates so the .fjgraph parses, but record() is a no-op and
// declare() doesn't write anything (composite falls back to ssr_output).
class RtReflectionPass : public RenderPass {
public:
    using Mode = RenderSettings::Ssr::Mode;

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    void set_accel_manager(AccelStructureManager* mgr) { accel_manager_ = mgr; }
    void set_mode(Mode m) { mode_ = m; }
    [[nodiscard]] Mode mode() const { return mode_; }
    [[nodiscard]] bool active() const { return mode_ == Mode::RayTraced && rt_pipeline_ != VK_NULL_HANDLE; }

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
    AccelStructureManager* accel_manager_{nullptr};

    Mode mode_{Mode::ScreenSpace};
    float intensity_{1.0F};

    // Cached sun light (set per-frame from the directional light scene node)
    glm::vec3 sun_direction_{0.0F, -1.0F, 0.0F};
    glm::vec3 sun_color_{1.0F};
    float sun_intensity_{1.5F};

    // RT pipeline. NULL_HANDLE when device lacks RT support — the pass
    // still exists but record() short-circuits.
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
};

} // namespace fjell
