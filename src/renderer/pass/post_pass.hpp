#pragma once

#include "core/named_value.hpp"
#include "gpu/owned.hpp"
#include "gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/post_layers.hpp"
#include "renderer/renderer_constants.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fjell {

class BindlessTextures;
class Buffer;
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
// write a material row's params: it writes each layer's values into a buffer
// of its own per frame in flight, one slice per draw, and pushes where the
// slice starts. The material row supplies the textures.
class PostPass : public RenderPass {
public:
    /// The format of what the viewport shows, which the editor overlays draw
    /// into after this pass.
    static constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R8G8B8A8_SRGB;

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


    // Per-viewport output image. Input descriptor sets are not owned per
    // viewport: record_into() acquires them from FrameContext::descriptor_cache
    // against the current source views.
    struct Images {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        /// The output as the GPU interface's texture.
        gpu::Owned<gpu::Texture> texture;
        VkExtent2D extent{};
        VkDescriptorSet imgui_descriptor{VK_NULL_HANDLE};
        /// Whether the pass has drawn into the image since it was made.
        bool written{false};

        void create(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
                     uint32_t width, uint32_t height);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
                     uint32_t width, uint32_t height);

        [[nodiscard]] VkDescriptorSet imgui_texture(VkSampler sampler);
        void invalidate_imgui_texture();
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
    [[nodiscard]] VkSampler output_sampler() const { return sampler_; }

private:
    /// A post shader's two pipelines. The first layer covers the image
    /// and draws opaque, as the one draw of the common case should; every
    /// layer after it blends over what is there by the weight it writes.
    struct MaterialPipeline {
        VkPipeline base{VK_NULL_HANDLE};
        VkPipeline over{VK_NULL_HANDLE};
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
    /// Resolve a layer's values and write them into this frame's buffer.
    /// Returns where they start, in floats.
    uint32_t write_values(uint32_t frame_index, const PostLayer& layer);

    /// What the template's set 2 samples. `bloom` is null when this
    /// viewport's graph builds no bloom.
    struct Inputs {
        VkImageView color{VK_NULL_HANDLE};
        VkImageView bloom{VK_NULL_HANDLE};
        VkImageView depth{VK_NULL_HANDLE};
        VkImageView normal{VK_NULL_HANDLE};
        VkImageView velocity{VK_NULL_HANDLE};
    };
    void record_into(const FrameContext& ctx, Images& imgs, const Inputs& inputs, float exposure,
                     std::span<const PostLayer> layers);

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    gpu::Device* gpu_device_{nullptr};
    VkDescriptorSetLayout global_set_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout material_set_layout_{VK_NULL_HANDLE};

    PipelineCache* pipeline_cache_{nullptr};
    BindlessTextures* bindless_{nullptr};

    // Resolution-independent (shared across all Images)
    VkSampler sampler_{VK_NULL_HANDLE};
    VkSampler input_sampler_{VK_NULL_HANDLE};
    VkSampler nearest_sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout input_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};

    std::unordered_map<uint32_t, MaterialPipeline> material_pipelines_;
    /// Entries already reported as unusable, so a broken shader is said once
    /// rather than every frame.
    std::unordered_set<uint32_t> reported_entries_;
    Fallback default_;
    TextureFallbacks texture_fallbacks_;
    bool ungraded_{false};

    /// This frame's layer values: host-written, read by the fragment
    /// shader at the pushed base. Reset in prepare_frame(), once the frame
    /// that last used the slot has finished.
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> value_buffers_;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> values_used_{};

    float exposure_{0.0f};
    float luminance_{0.0f};
    bool auto_exposure_{false};
    float adaptation_speed_{1.5f};
    float min_ev_{-5.0f};
    float max_ev_{5.0f};
};

} // namespace fjell
