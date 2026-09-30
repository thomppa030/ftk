#pragma once

#include "gpu/owned.hpp"
#include "gpu/texture.hpp"
#include "gpu/vulkan/translate.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class Device;
namespace gpu {
class Device;
}

class OffscreenPass {
public:
    /// `gpu` is the GPU interface's device, which is handed every image
    /// made here as a texture.
    OffscreenPass(Device& device, gpu::Device& gpu, VmaAllocator allocator,
                  uint32_t width, uint32_t height, gpu::Samples samples = gpu::Samples::x1);
    ~OffscreenPass();

    OffscreenPass(const OffscreenPass&) = delete;
    OffscreenPass& operator=(const OffscreenPass&) = delete;
    OffscreenPass(OffscreenPass&&) = delete;
    OffscreenPass& operator=(OffscreenPass&&) = delete;

    void resize(uint32_t width, uint32_t height);

    // Returns the single-sample color view (resolve target when MSAA, direct when 1x).
    // Use this for post-process sampling and ImGui display.
    [[nodiscard]] VkImageView color_view() const { return resolve_color_view_ ? resolve_color_view_ : color_view_; }

    // Returns the MSAA color view for rendering into (same as color_view when 1x).
    [[nodiscard]] VkImageView msaa_color_view() const { return color_view_; }

    // Returns the single-sample depth view (resolve target when MSAA, direct when 1x).
    [[nodiscard]] VkImageView depth_view() const { return resolve_depth_view_ ? resolve_depth_view_ : depth_view_; }

    // Returns the MSAA depth view for rendering into (same as depth_view when 1x).
    [[nodiscard]] VkImageView msaa_depth_view() const { return depth_view_; }
    [[nodiscard]] VkImage color_image() const { return resolve_color_image_ ? resolve_color_image_ : color_image_; }
    [[nodiscard]] VkImage msaa_color_image() const { return color_image_; }
    [[nodiscard]] VkImage depth_image() const { return resolve_depth_image_ ? resolve_depth_image_ : depth_image_; }
    [[nodiscard]] VkImage msaa_depth_image() const { return depth_image_; }

    /// The sea's own depth buffer, which it may write so that a folded crest
    /// hides the water behind it. Seeded from the scene depth each frame it is
    /// used, so it holds the opaque scene with the water laid over it.
    [[nodiscard]] VkImageView water_depth_view() const { return water_depth_view_; }
    [[nodiscard]] VkImage water_depth_image() const { return water_depth_image_; }

    /// That same depth as something to sample: the resolve target under MSAA,
    /// the water depth itself at one sample. Only holds anything in a frame the
    /// water drew — ask for "surface_depth" by name, which falls back to the
    /// scene depth when it did not.
    [[nodiscard]] VkImageView surface_depth_view() const {
        return resolve_water_depth_view_ ? resolve_water_depth_view_ : water_depth_view_;
    }
    [[nodiscard]] VkImage surface_depth_image() const {
        return resolve_water_depth_image_ ? resolve_water_depth_image_ : water_depth_image_;
    }
    /// The resolve target alone, null at one sample, for the scope that fills it.
    [[nodiscard]] VkImageView resolve_water_depth_view() const { return resolve_water_depth_view_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] gpu::Samples samples() const { return gpu::vulkan::from_vk(msaa_samples_); }
    [[nodiscard]] VkDescriptorSet imgui_texture();
    [[nodiscard]] VkDescriptorSet depth_imgui_texture();
    void invalidate_imgui_texture();
    void invalidate_depth_imgui_texture();

    static constexpr VkFormat COLOR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat MR_FORMAT = VK_FORMAT_R8G8_UNORM;
    static constexpr VkFormat NORMAL_FORMAT = VK_FORMAT_R16G16_SFLOAT;
    static constexpr VkFormat VELOCITY_FORMAT = VK_FORMAT_R16G16_SFLOAT;
    static constexpr VkFormat DISTORTION_FORMAT = VK_FORMAT_R16G16_SFLOAT;

    // Returns the single-sample metallic/roughness view (resolve target when MSAA, direct when 1x).
    [[nodiscard]] VkImageView mr_view() const { return resolve_mr_view_ ? resolve_mr_view_ : mr_view_; }

    // Returns the MSAA metallic/roughness view for rendering into (same as mr_view when 1x).
    [[nodiscard]] VkImageView msaa_mr_view() const { return mr_view_; }

    [[nodiscard]] VkImage mr_image() const { return resolve_mr_image_ ? resolve_mr_image_ : mr_image_; }
    [[nodiscard]] VkImage msaa_mr_image() const { return mr_image_; }

    // Returns the single-sample normal view (resolve target when MSAA, direct when 1x).
    [[nodiscard]] VkImageView normal_view() const { return resolve_normal_view_ ? resolve_normal_view_ : normal_view_; }

    // Returns the MSAA normal view for rendering into (same as normal_view when 1x).
    [[nodiscard]] VkImageView msaa_normal_view() const { return normal_view_; }

    [[nodiscard]] VkImage normal_image() const { return resolve_normal_image_ ? resolve_normal_image_ : normal_image_; }
    [[nodiscard]] VkImage msaa_normal_image() const { return normal_image_; }

    // Returns the single-sample velocity view (resolve target when MSAA, direct when 1x).
    [[nodiscard]] VkImageView velocity_view() const { return resolve_velocity_view_ ? resolve_velocity_view_ : velocity_view_; }

    // Returns the MSAA velocity view for rendering into (same as velocity_view when 1x).
    [[nodiscard]] VkImageView msaa_velocity_view() const { return velocity_view_; }

    [[nodiscard]] VkImage velocity_image() const { return resolve_velocity_image_ ? resolve_velocity_image_ : velocity_image_; }
    [[nodiscard]] VkImage msaa_velocity_image() const { return velocity_image_; }

    // Screen color copy — snapshot of resolved color after opaques, before transparents.
    [[nodiscard]] VkImage screen_color_image() const { return screen_color_image_; }
    [[nodiscard]] VkImageView screen_color_view() const { return screen_color_view_; }
    [[nodiscard]] bool screen_color_needs_init() const { return screen_color_needs_init_; }
    void mark_screen_color_initialized() { screen_color_needs_init_ = false; }

    // Pre-baked descriptors (e.g. ImGui texture, post-process inputs) hold
    // color/resolve_color at SHADER_READ_ONLY. The framegraph's slice
    // tracker resets to UNDEFINED at begin_frame, but the validator checks
    // image layouts at submit time against descriptor expectations. On the
    // very first frame — before any pass has written to color — the image
    // is still UNDEFINED, which trips the validator if a draw in the same
    // submit references a SHADER_READ-bound descriptor for it. One-shot
    // UNDEFINED -> SHADER_READ_ONLY transition makes the first frame
    // match what later frames already exhibit.
    [[nodiscard]] bool color_needs_init() const { return color_needs_init_; }
    void mark_color_initialized() { color_needs_init_ = false; }

    // Distortion buffer — UV offsets for refraction effects.
    [[nodiscard]] VkImage distortion_image() const { return distortion_image_; }
    [[nodiscard]] VkImageView distortion_view() const { return distortion_view_; }

    // The images above as the GPU interface's textures, picked the same way:
    // the resolve target under MSAA, the image itself at one sample.
    [[nodiscard]] gpu::Texture color_texture() const { return pick(textures_.resolve_color, textures_.color); }
    [[nodiscard]] gpu::Texture msaa_color_texture() const { return textures_.color; }
    [[nodiscard]] gpu::Texture depth_texture() const { return pick(textures_.resolve_depth, textures_.depth); }
    [[nodiscard]] gpu::Texture msaa_depth_texture() const { return textures_.depth; }
    [[nodiscard]] gpu::Texture water_depth_texture() const { return textures_.water_depth; }
    [[nodiscard]] gpu::Texture surface_depth_texture() const {
        return pick(textures_.resolve_water_depth, textures_.water_depth);
    }
    [[nodiscard]] gpu::Texture mr_texture() const { return pick(textures_.resolve_mr, textures_.mr); }
    [[nodiscard]] gpu::Texture msaa_mr_texture() const { return textures_.mr; }
    [[nodiscard]] gpu::Texture normal_texture() const { return pick(textures_.resolve_normal, textures_.normal); }
    [[nodiscard]] gpu::Texture msaa_normal_texture() const { return textures_.normal; }
    [[nodiscard]] gpu::Texture velocity_texture() const {
        return pick(textures_.resolve_velocity, textures_.velocity);
    }
    [[nodiscard]] gpu::Texture msaa_velocity_texture() const { return textures_.velocity; }
    [[nodiscard]] gpu::Texture screen_color_texture() const { return textures_.screen_color; }
    [[nodiscard]] gpu::Texture distortion_texture() const { return textures_.distortion; }

private:
    void create(uint32_t width, uint32_t height);
    void cleanup_images();

    // The resolve target when there is one, else the image rendered into.
    [[nodiscard]] static gpu::Texture pick(const gpu::Owned<gpu::Texture>& resolve,
                                           const gpu::Owned<gpu::Texture>& image) {
        return resolve ? resolve.get() : image.get();
    }

    // Every image below as a texture, handed to the interface where it is
    // made and forgotten where it is destroyed.
    struct Textures {
        gpu::Owned<gpu::Texture> color, resolve_color;
        gpu::Owned<gpu::Texture> depth, resolve_depth;
        gpu::Owned<gpu::Texture> water_depth, resolve_water_depth;
        gpu::Owned<gpu::Texture> mr, resolve_mr;
        gpu::Owned<gpu::Texture> normal, resolve_normal;
        gpu::Owned<gpu::Texture> velocity, resolve_velocity;
        gpu::Owned<gpu::Texture> screen_color;
        gpu::Owned<gpu::Texture> distortion;
    };

    Device& device_;
    gpu::Device& gpu_;
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkSampleCountFlagBits msaa_samples_{VK_SAMPLE_COUNT_1_BIT};

    // Color render target (MSAA when samples > 1)
    VkImage color_image_{VK_NULL_HANDLE};
    VmaAllocation color_allocation_{VK_NULL_HANDLE};
    VkImageView color_view_{VK_NULL_HANDLE};

    // Resolve target (single-sample, only created when MSAA)
    VkImage resolve_color_image_{VK_NULL_HANDLE};
    VmaAllocation resolve_color_allocation_{VK_NULL_HANDLE};
    VkImageView resolve_color_view_{VK_NULL_HANDLE};

    VkSampler sampler_{VK_NULL_HANDLE};
    VkSampler depth_sampler_{VK_NULL_HANDLE};

    // Depth render target (MSAA when samples > 1)
    VkImage depth_image_{VK_NULL_HANDLE};
    VmaAllocation depth_allocation_{VK_NULL_HANDLE};
    VkImageView depth_view_{VK_NULL_HANDLE};

    // Depth resolve target (single-sample, only created when MSAA)
    VkImage resolve_depth_image_{VK_NULL_HANDLE};
    VmaAllocation resolve_depth_allocation_{VK_NULL_HANDLE};
    VkImageView resolve_depth_view_{VK_NULL_HANDLE};

    VkImage water_depth_image_{VK_NULL_HANDLE};
    VmaAllocation water_depth_allocation_{VK_NULL_HANDLE};
    VkImageView water_depth_view_{VK_NULL_HANDLE};
    VkImage resolve_water_depth_image_{VK_NULL_HANDLE};
    VmaAllocation resolve_water_depth_allocation_{VK_NULL_HANDLE};
    VkImageView resolve_water_depth_view_{VK_NULL_HANDLE};

    // Metallic/roughness render target (MSAA when samples > 1)
    VkImage mr_image_{VK_NULL_HANDLE};
    VmaAllocation mr_allocation_{VK_NULL_HANDLE};
    VkImageView mr_view_{VK_NULL_HANDLE};

    // Metallic/roughness resolve target (single-sample, only created when MSAA)
    VkImage resolve_mr_image_{VK_NULL_HANDLE};
    VmaAllocation resolve_mr_allocation_{VK_NULL_HANDLE};
    VkImageView resolve_mr_view_{VK_NULL_HANDLE};

    // Normal render target — octahedral-encoded world normals (MSAA when samples > 1)
    VkImage normal_image_{VK_NULL_HANDLE};
    VmaAllocation normal_allocation_{VK_NULL_HANDLE};
    VkImageView normal_view_{VK_NULL_HANDLE};

    // Normal resolve target (single-sample, only created when MSAA)
    VkImage resolve_normal_image_{VK_NULL_HANDLE};
    VmaAllocation resolve_normal_allocation_{VK_NULL_HANDLE};
    VkImageView resolve_normal_view_{VK_NULL_HANDLE};

    // Velocity render target — screen-space motion vectors (MSAA when samples > 1)
    VkImage velocity_image_{VK_NULL_HANDLE};
    VmaAllocation velocity_allocation_{VK_NULL_HANDLE};
    VkImageView velocity_view_{VK_NULL_HANDLE};

    // Velocity resolve target (single-sample, only created when MSAA)
    VkImage resolve_velocity_image_{VK_NULL_HANDLE};
    VmaAllocation resolve_velocity_allocation_{VK_NULL_HANDLE};
    VkImageView resolve_velocity_view_{VK_NULL_HANDLE};

    // Screen color copy (single-sample, for SCREEN_TEXTURE in transparent shaders)
    VkImage screen_color_image_{VK_NULL_HANDLE};
    VmaAllocation screen_color_allocation_{VK_NULL_HANDLE};
    VkImageView screen_color_view_{VK_NULL_HANDLE};
    bool screen_color_needs_init_{true};
    bool color_needs_init_{true};

    // Distortion buffer (single-sample, RG16F UV offsets for refraction)
    VkImage distortion_image_{VK_NULL_HANDLE};
    VmaAllocation distortion_allocation_{VK_NULL_HANDLE};
    VkImageView distortion_view_{VK_NULL_HANDLE};

    VkExtent2D extent_{};
    Textures textures_;

    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};
    VkDescriptorSet depth_imgui_descriptor_{VK_NULL_HANDLE};
};

} // namespace fjell
