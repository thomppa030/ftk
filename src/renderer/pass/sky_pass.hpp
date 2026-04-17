#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace fjell {

// Draws a solid color or vertical gradient behind all geometry.
// Renders into the existing color pass (between begin/end rendering)
// using reverse depth test so it only fills pixels with no geometry.
class SkyPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    [[nodiscard]] const glm::vec3& top_color() const { return top_color_; }
    [[nodiscard]] const glm::vec3& bottom_color() const { return bottom_color_; }

    // Sky mode matches the shader's push.sky_mode:
    //   0 = gradient, 1 = HDRI cubemap, 2 = procedural atmospheric scattering.
    enum class Mode : int { Gradient = 0, Hdri = 1, Atmosphere = 2 };
    [[nodiscard]] Mode mode() const { return mode_; }

    [[nodiscard]] float atmosphere_rayleigh_scale() const { return atm_rayleigh_scale_; }
    [[nodiscard]] float atmosphere_mie_scale() const { return atm_mie_scale_; }
    [[nodiscard]] float atmosphere_mie_g() const { return atm_mie_g_; }
    [[nodiscard]] float atmosphere_mie_coeff() const { return atm_mie_coeff_; }
    [[nodiscard]] float atmosphere_sun_intensity() const { return atm_sun_intensity_; }
    [[nodiscard]] float atmosphere_hdri_floor() const { return atm_hdri_floor_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    glm::vec3 top_color_{0.15f, 0.25f, 0.45f};
    glm::vec3 bottom_color_{0.8f, 0.4f, 0.1f};
    Mode mode_{Mode::Gradient};

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
