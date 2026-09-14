#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/renderer_constants.hpp"
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

    /// Per-viewport image and descriptor state. Owned by
    /// PostProcessResources, one instance per viewport. Pass singleton
    /// owns shared state (samplers, set layout, RT pipeline, SBT).
    struct Images {
        VkImage out_image{VK_NULL_HANDLE};
        VmaAllocation out_alloc{VK_NULL_HANDLE};
        VkImageView out_view{VK_NULL_HANDLE};
        VkExtent2D out_extent{};
        VkImageLayout out_layout{VK_IMAGE_LAYOUT_UNDEFINED};

        // One uniform buffer and one trace set per frame in flight. The UBO
        // is host-written every frame and the set binds the slot's TLAS and
        // per-instance table through update-after-bind, so a frame still
        // pending on the GPU must not share either with the frame being
        // recorded.
        std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> reflection_ubo;
        VkDescriptorPool desc_pool{VK_NULL_HANDLE};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> trace_set{};
        bool first_use{true};

        void create(VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout set_layout,
                     uint32_t width, uint32_t height);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
                     VkDescriptorSetLayout set_layout,
                     uint32_t width, uint32_t height);
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

    [[nodiscard]] VkDescriptorSetLayout trace_set_layout() const { return trace_set_layout_; }

private:
    void record_into(const FrameContext& ctx, Images& imgs);

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

    // Shared (pass-singleton) state. NULL when device lacks RT support.
    VkSampler linear_sampler_{VK_NULL_HANDLE};
    VkSampler nearest_sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout trace_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout rt_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline rt_pipeline_{VK_NULL_HANDLE};

    // Shader binding table (shared)
    std::unique_ptr<Buffer> sbt_buffer_;
    VkStridedDeviceAddressRegionKHR sbt_raygen_{};
    VkStridedDeviceAddressRegionKHR sbt_miss_{};
    VkStridedDeviceAddressRegionKHR sbt_hit_{};
    VkStridedDeviceAddressRegionKHR sbt_callable_{};
};

} // namespace fjell
