#pragma once

#include "gpu/format.hpp"
#include "gpu/owned.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace fjell {

namespace gpu {
class Device;
}

/// The images a view renders into: colour, depth, the surface targets beside
/// colour (metallic/roughness, normal, motion), the sea's own depth, the copy
/// of the screen transparents sample and the distortion offsets, with a
/// single-sample resolve target for each drawn one under MSAA. The device
/// makes them; a resize hands the old ones back, and the device keeps them for
/// the frames still using them.
class OffscreenPass {
public:
    OffscreenPass(gpu::Device& gpu, uint32_t width, uint32_t height,
                  gpu::Samples samples = gpu::Samples::x1);
    ~OffscreenPass();

    OffscreenPass(const OffscreenPass&) = delete;
    OffscreenPass& operator=(const OffscreenPass&) = delete;
    OffscreenPass(OffscreenPass&&) = delete;
    OffscreenPass& operator=(OffscreenPass&&) = delete;

    void resize(uint32_t width, uint32_t height);

    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] gpu::Samples samples() const { return samples_; }
    /// The colour target as ImGui shows it; made on first ask, and let go
    /// with the target or its resize once the frames drawing it are done.
    [[nodiscard]] VkDescriptorSet imgui_texture();
    /// Lets that go now, for an owner shutting down before ImGui does.
    void release_imgui_texture();

    static constexpr gpu::Format COLOR_FORMAT = gpu::Format::rgba16_float;
    static constexpr gpu::Format MR_FORMAT = gpu::Format::rg8_unorm;
    static constexpr gpu::Format NORMAL_FORMAT = gpu::Format::rg16_float;
    static constexpr gpu::Format VELOCITY_FORMAT = gpu::Format::rg16_float;
    static constexpr gpu::Format DISTORTION_FORMAT = gpu::Format::rg16_float;
    /// Depth without stencil: the scene needs none, and every GPU the engine
    /// runs on renders to it, Apple's included, which lack 24-bit depth.
    static constexpr gpu::Format DEPTH_FORMAT = gpu::Format::d32_float;

    // Each image as the GPU interface's texture: the resolve target under
    // MSAA, the image itself at one sample, and the image rendered into as
    // `msaa_*`.
    [[nodiscard]] gpu::Texture color_texture() const { return pick(textures_.resolve_color, textures_.color); }
    [[nodiscard]] gpu::Texture msaa_color_texture() const { return textures_.color; }
    [[nodiscard]] gpu::Texture depth_texture() const { return pick(textures_.resolve_depth, textures_.depth); }
    [[nodiscard]] gpu::Texture msaa_depth_texture() const { return textures_.depth; }
    /// The sea's own depth buffer, which it may write so that a folded crest
    /// hides the water behind it. Seeded from the scene depth each frame it is
    /// used, so it holds the opaque scene with the water laid over it.
    [[nodiscard]] gpu::Texture water_depth_texture() const { return textures_.water_depth; }
    /// That same depth as something to sample: the resolve target under MSAA,
    /// the water depth itself at one sample. Only holds anything in a frame the
    /// water drew — ask for "surface_depth" by name, which falls back to the
    /// scene depth when it did not.
    [[nodiscard]] gpu::Texture surface_depth_texture() const {
        return pick(textures_.resolve_water_depth, textures_.water_depth);
    }
    [[nodiscard]] gpu::Texture mr_texture() const { return pick(textures_.resolve_mr, textures_.mr); }
    [[nodiscard]] gpu::Texture msaa_mr_texture() const { return textures_.mr; }
    /// Octahedral-encoded world normals.
    [[nodiscard]] gpu::Texture normal_texture() const { return pick(textures_.resolve_normal, textures_.normal); }
    [[nodiscard]] gpu::Texture msaa_normal_texture() const { return textures_.normal; }
    /// Screen-space motion.
    [[nodiscard]] gpu::Texture velocity_texture() const {
        return pick(textures_.resolve_velocity, textures_.velocity);
    }
    [[nodiscard]] gpu::Texture msaa_velocity_texture() const { return textures_.velocity; }
    /// The resolved colour after the opaques, before the transparents, for
    /// SCREEN_TEXTURE.
    [[nodiscard]] gpu::Texture screen_color_texture() const { return textures_.screen_color; }
    /// Screen-space offsets for refraction.
    [[nodiscard]] gpu::Texture distortion_texture() const { return textures_.distortion; }

private:
    void create(uint32_t width, uint32_t height);

    // The resolve target when there is one, else the image rendered into.
    [[nodiscard]] static gpu::Texture pick(const gpu::Owned<gpu::Texture>& resolve,
                                           const gpu::Owned<gpu::Texture>& image) {
        return resolve ? resolve.get() : image.get();
    }

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

    gpu::Device& gpu_;
    gpu::Samples samples_{gpu::Samples::x1};
    /// Names this target's images in debuggers and validation messages.
    std::string name_;
    /// Linear, clamped: how ImGui samples the colour target.
    gpu::Sampler sampler_{};
    VkExtent2D extent_{};
    Textures textures_;
    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};
};

} // namespace fjell
