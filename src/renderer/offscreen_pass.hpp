#pragma once

#include "ftk/gpu/format.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/texture.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace ftk::gpu {
class Device;
} // namespace ftk::gpu

namespace fjell {

/// The images a view renders into: colour, depth, the surface targets beside
/// colour (metallic/roughness, normal, motion), the sea's own depth, the copy
/// of the screen transparents sample and the distortion offsets, with a
/// single-sample resolve target for each drawn one under MSAA. The device
/// makes them; a resize hands the old ones back, and the device keeps them for
/// the frames still using them.
class OffscreenPass {
public:
    /// What a target holds: every image a view of a scene renders into, or
    /// the colour target alone, for one that draws nothing else (game UI).
    /// A colour target's other textures are invalid.
    enum class Contents : uint8_t {
        scene,
        color,
    };

    OffscreenPass(ftk::gpu::Device& gpu, uint32_t width, uint32_t height,
                  ftk::gpu::Samples samples = ftk::gpu::Samples::x1, Contents contents = Contents::scene);

    OffscreenPass(const OffscreenPass&) = delete;
    OffscreenPass& operator=(const OffscreenPass&) = delete;
    OffscreenPass(OffscreenPass&&) = delete;
    OffscreenPass& operator=(OffscreenPass&&) = delete;

    void resize(uint32_t width, uint32_t height);

    [[nodiscard]] glm::uvec2 extent() const { return extent_; }
    [[nodiscard]] ftk::gpu::Samples samples() const { return samples_; }

    static constexpr ftk::gpu::Format COLOR_FORMAT = ftk::gpu::Format::rgba16_float;
    static constexpr ftk::gpu::Format MR_FORMAT = ftk::gpu::Format::rg8_unorm;
    static constexpr ftk::gpu::Format NORMAL_FORMAT = ftk::gpu::Format::rg16_float;
    static constexpr ftk::gpu::Format VELOCITY_FORMAT = ftk::gpu::Format::rg16_float;
    static constexpr ftk::gpu::Format DISTORTION_FORMAT = ftk::gpu::Format::rg16_float;
    /// Depth without stencil: the scene needs none, and every GPU the engine
    /// runs on renders to it, Apple's included, which lack 24-bit depth.
    static constexpr ftk::gpu::Format DEPTH_FORMAT = ftk::gpu::Format::d32_float;

    // Each image as the GPU interface's texture: the resolve target under
    // MSAA, the image itself at one sample, and the image rendered into as
    // `msaa_*`.
    [[nodiscard]] ftk::gpu::Texture color_texture() const { return pick(textures_.resolve_color, textures_.color); }
    [[nodiscard]] ftk::gpu::Texture msaa_color_texture() const { return textures_.color; }
    [[nodiscard]] ftk::gpu::Texture depth_texture() const { return pick(textures_.resolve_depth, textures_.depth); }
    [[nodiscard]] ftk::gpu::Texture msaa_depth_texture() const { return textures_.depth; }
    /// The sea's own depth buffer, which it may write so that a folded crest
    /// hides the water behind it. Seeded from the scene depth each frame it is
    /// used, so it holds the opaque scene with the water laid over it.
    [[nodiscard]] ftk::gpu::Texture water_depth_texture() const { return textures_.water_depth; }
    /// That same depth as something to sample: the resolve target under MSAA,
    /// the water depth itself at one sample. Only holds anything in a frame the
    /// water drew — ask for "surface_depth" by name, which falls back to the
    /// scene depth when it did not.
    [[nodiscard]] ftk::gpu::Texture surface_depth_texture() const {
        return pick(textures_.resolve_water_depth, textures_.water_depth);
    }
    [[nodiscard]] ftk::gpu::Texture mr_texture() const { return pick(textures_.resolve_mr, textures_.mr); }
    [[nodiscard]] ftk::gpu::Texture msaa_mr_texture() const { return textures_.mr; }
    /// Octahedral-encoded world normals.
    [[nodiscard]] ftk::gpu::Texture normal_texture() const { return pick(textures_.resolve_normal, textures_.normal); }
    [[nodiscard]] ftk::gpu::Texture msaa_normal_texture() const { return textures_.normal; }
    /// Screen-space motion.
    [[nodiscard]] ftk::gpu::Texture velocity_texture() const {
        return pick(textures_.resolve_velocity, textures_.velocity);
    }
    [[nodiscard]] ftk::gpu::Texture msaa_velocity_texture() const { return textures_.velocity; }
    /// The resolved colour after the opaques, before the transparents, for
    /// SCREEN_TEXTURE.
    [[nodiscard]] ftk::gpu::Texture screen_color_texture() const { return textures_.screen_color; }
    /// Screen-space offsets for refraction.
    [[nodiscard]] ftk::gpu::Texture distortion_texture() const { return textures_.distortion; }

private:
    void create(uint32_t width, uint32_t height);

    // The resolve target when there is one, else the image rendered into.
    [[nodiscard]] static ftk::gpu::Texture pick(const ftk::gpu::Owned<ftk::gpu::Texture>& resolve,
                                           const ftk::gpu::Owned<ftk::gpu::Texture>& image) {
        return resolve ? resolve.get() : image.get();
    }

    struct Textures {
        ftk::gpu::Owned<ftk::gpu::Texture> color, resolve_color;
        ftk::gpu::Owned<ftk::gpu::Texture> depth, resolve_depth;
        ftk::gpu::Owned<ftk::gpu::Texture> water_depth, resolve_water_depth;
        ftk::gpu::Owned<ftk::gpu::Texture> mr, resolve_mr;
        ftk::gpu::Owned<ftk::gpu::Texture> normal, resolve_normal;
        ftk::gpu::Owned<ftk::gpu::Texture> velocity, resolve_velocity;
        ftk::gpu::Owned<ftk::gpu::Texture> screen_color;
        ftk::gpu::Owned<ftk::gpu::Texture> distortion;
    };

    ftk::gpu::Device& gpu_;
    ftk::gpu::Samples samples_{ftk::gpu::Samples::x1};
    Contents contents_{Contents::scene};
    /// Names this target's images in debuggers and validation messages.
    std::string name_;
    glm::uvec2 extent_{0, 0};
    Textures textures_;
};

} // namespace fjell
