#pragma once

#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "environment/environment_state.hpp"
#include "environment/environment_ubo.hpp"

namespace fjell {

class SkyPass;

/// Generates a sky gradient cubemap (64x64 per face, mipmapped) and a
/// split-sum BRDF integration LUT (512x512 RG16F) for IBL.
class SkyCubemapPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void register_resources(ResourceRegistry& registry) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    // Legacy create — forwards to standard create (needs command pool + queue)
    void create(VkDevice device, VmaAllocator allocator,
                VkCommandPool command_pool, VkQueue queue,
                VkPipelineCache pipeline_cache = VK_NULL_HANDLE);

    /// Set sky gradient colors. Marks dirty so next generate() re-renders.
    void set_colors(const glm::vec3& top, const glm::vec3& bottom);

    /// Configure atmospheric scattering state. When enabled, the cubemap
    /// is regenerated with the procedural model instead of the gradient,
    /// so IBL sampling matches what the sky pass renders.
    void set_atmosphere(bool enabled, const glm::vec3& sun_dir,
                        const glm::vec3& sun_color, float sun_intensity,
                        float rayleigh_scale, float mie_scale,
                        float mie_g, float mie_coeff);

    /// Dispatch the cubemap compute shader and generate mips.
    /// Only does work if dirty.
    void generate(VkCommandBuffer cmd);
    /// Render the active FJSL sky into the cubemap with its compute variant,
    /// so the sky that is drawn is the sky that lights. Rebuilds its pipeline
    /// when SkyPass compiles a new shader.
    void generate_fjsl(const FrameContext& ctx);

    [[nodiscard]] VkImageView cubemap_view() const {
        return hdri_cubemap_view_ != VK_NULL_HANDLE ? hdri_cubemap_view_ : cubemap_view_;
    }
    [[nodiscard]] VkSampler cubemap_sampler() const {
        return hdri_cubemap_sampler_ != VK_NULL_HANDLE ? hdri_cubemap_sampler_ : cubemap_sampler_;
    }
    [[nodiscard]] VkImageView brdf_lut_view() const { return brdf_lut_view_; }
    [[nodiscard]] VkSampler brdf_lut_sampler() const { return brdf_lut_sampler_; }

    [[nodiscard]] bool dirty() const { return dirty_; }
    [[nodiscard]] bool has_hdri() const { return hdri_cubemap_view_ != VK_NULL_HANDLE; }

    /// Monotonic counter — bumped whenever the sky cubemap view/sampler
    /// identity changes (HDRI load, HDRI clear). Consumers (DDGI, SSR) that
    /// cache the cubemap in their own descriptor sets compare their last-seen
    /// version and rewrite when stale.
    [[nodiscard]] uint32_t binding_version() const { return binding_version_; }

    /// Load an HDRI equirectangular image, convert to cubemap, and prefilter.
    /// Path must be absolute or resolvable from the current working directory.
    void load_hdri(const std::string& path);

    void set_project(const class Project* p) { project_ = p; }

private:
    void create_cubemap();
    void create_brdf_lut();
    void create_cubemap_pipeline();
    // Move mip 0 of the cubemap into GENERAL for a compute write.
    void begin_cubemap_write(VkCommandBuffer cmd);
    void ensure_fjsl_pipeline();
    void destroy_fjsl_pipeline();
    void create_brdf_pipeline();
    void create_hdri_pipelines();
    void generate_brdf_lut();
    void generate_mips(VkCommandBuffer cmd);
    void generate_mips_hdri(VkCommandBuffer cmd);
    void destroy_hdri();

    static constexpr uint32_t CUBEMAP_SIZE = 64;
    static constexpr uint32_t HDRI_CUBEMAP_SIZE = 512;
    static constexpr uint32_t HDRI_MIP_LEVELS = 10; // floor(log2(512)) + 1
    static constexpr uint32_t BRDF_LUT_SIZE = 512;
    static constexpr uint32_t MIP_LEVELS = 7; // floor(log2(64)) + 1

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};

    // Cubemap
    VkImage cubemap_image_{VK_NULL_HANDLE};
    VmaAllocation cubemap_memory_{VK_NULL_HANDLE};
    VkImageView cubemap_view_{VK_NULL_HANDLE};        // CUBE view for sampling
    VkImageView cubemap_storage_view_{VK_NULL_HANDLE}; // 2D_ARRAY view for compute storage (mip 0)
    VkSampler cubemap_sampler_{VK_NULL_HANDLE};

    // Cubemap compute
    VkDescriptorSetLayout cubemap_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool cubemap_pool_{VK_NULL_HANDLE};
    VkDescriptorSet cubemap_set_{VK_NULL_HANDLE};
    VkPipelineLayout cubemap_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline cubemap_pipeline_{VK_NULL_HANDLE};

    // BRDF LUT
    VkImage brdf_lut_image_{VK_NULL_HANDLE};
    VmaAllocation brdf_lut_memory_{VK_NULL_HANDLE};
    VkImageView brdf_lut_view_{VK_NULL_HANDLE};
    VkSampler brdf_lut_sampler_{VK_NULL_HANDLE};

    // BRDF compute
    VkDescriptorSetLayout brdf_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool brdf_pool_{VK_NULL_HANDLE};
    VkDescriptorSet brdf_set_{VK_NULL_HANDLE};
    VkPipelineLayout brdf_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline brdf_pipeline_{VK_NULL_HANDLE};

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
    bool first_generate_{true};

    // FJSL sky: the sky shader's compute variant over the six faces. Binds
    // the global set, the bindless set and the storage image.
    PassResources resources_{};
    SkyPass* sky_pass_{nullptr};
    bool fjsl_active_{false};
    VkPipelineLayout fjsl_layout_{VK_NULL_HANDLE};
    VkPipeline fjsl_pipeline_{VK_NULL_HANDLE};
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
    uint32_t binding_version_{0};
    ResourceRegistry* registry_{nullptr};
    const class Project* project_{nullptr};

    // ── HDRI environment map ──────────────────────────────────────────
    std::string loaded_hdri_path_;

    // Equirectangular source (2D, R32G32B32A32_SFLOAT)
    VkImage equirect_image_{VK_NULL_HANDLE};
    VmaAllocation equirect_memory_{VK_NULL_HANDLE};
    VkImageView equirect_view_{VK_NULL_HANDLE};
    VkSampler equirect_sampler_{VK_NULL_HANDLE};

    // HDRI cubemap (512x512, 10 mips, prefiltered)
    VkImage hdri_cubemap_image_{VK_NULL_HANDLE};
    VmaAllocation hdri_cubemap_memory_{VK_NULL_HANDLE};
    VkImageView hdri_cubemap_view_{VK_NULL_HANDLE};       // CUBE view (all mips)
    VkImageView hdri_storage_view_{VK_NULL_HANDLE};       // 2D_ARRAY (mip 0, for equirect conversion)
    VkSampler hdri_cubemap_sampler_{VK_NULL_HANDLE};

    // Per-mip storage views for prefilter output
    std::array<VkImageView, HDRI_MIP_LEVELS> hdri_mip_views_{};

    // Equirect-to-cube pipeline
    VkDescriptorSetLayout equirect_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool equirect_pool_{VK_NULL_HANDLE};
    VkDescriptorSet equirect_set_{VK_NULL_HANDLE};
    VkPipelineLayout equirect_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline equirect_pipeline_{VK_NULL_HANDLE};

    // Prefilter pipeline
    VkDescriptorSetLayout prefilter_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool prefilter_pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, HDRI_MIP_LEVELS> prefilter_sets_{};
    VkPipelineLayout prefilter_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline prefilter_pipeline_{VK_NULL_HANDLE};

    bool hdri_pipelines_created_{false};
};

} // namespace fjell
