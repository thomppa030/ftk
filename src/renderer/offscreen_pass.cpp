#include "renderer/offscreen_pass.hpp"

#include <gpu/device.hpp>

#include <optional>
#include <stdexcept>

namespace fjell {

OffscreenPass::OffscreenPass(gpu::Device& gpu, uint32_t width, uint32_t height, gpu::Samples samples,
                             Contents contents)
    : gpu_{gpu}, samples_{samples}, contents_{contents} {
    static uint32_t made = 0;
    name_ = "offscreen#" + std::to_string(made++);
    create(width, height);
}

void OffscreenPass::resize(uint32_t width, uint32_t height) {
    if (extent_.x == width && extent_.y == height) return;
    create(width, height);
}

void OffscreenPass::create(uint32_t width, uint32_t height) {
    extent_ = {width, height};
    const bool msaa = samples_ != gpu::Samples::x1;

    // Every image of this target; replacing the old ones hands them back to
    // the device, which keeps them for the frames still drawing with them.
    auto make = [&](std::string_view what, gpu::Format format, gpu::Samples samples, gpu::TextureUses use,
                    gpu::Queues queues = gpu::Queues::graphics,
                    std::optional<gpu::Clear> initial = std::nullopt) {
        const std::string name = name_ + "." + std::string(what);
        auto texture = gpu_.create(gpu::TextureDesc{
            .format = format,
            .width = width,
            .height = height,
            .samples = samples,
            .use = use,
            .queues = queues,
            .initial = initial,
            .name = name,
        });
        if (!texture) throw std::runtime_error("Offscreen target: " + texture.error());
        return std::move(*texture);
    };
    using Use = gpu::TextureUse;
    const gpu::TextureUses sampled_target = Use::color_target | Use::sampled;

    // Colour. The single-sample one is sampled and written as storage by the
    // post passes; the descriptors that show it (ImGui, the post inputs) may
    // be drawn before anything renders into it, so it starts cleared and
    // ready to sample.
    const gpu::TextureUses resolved_color = sampled_target | Use::storage;
    const gpu::Clear black{0.0f, 0.0f, 0.0f, 0.0f};
    if (msaa) {
        textures_.color = make("color", COLOR_FORMAT, samples_, Use::color_target);
        textures_.resolve_color =
            make("resolve_color", COLOR_FORMAT, gpu::Samples::x1, resolved_color, gpu::Queues::graphics, black);
    } else {
        textures_.color = make("color", COLOR_FORMAT, samples_, resolved_color, gpu::Queues::graphics, black);
        textures_.resolve_color = {};
    }
    if (contents_ == Contents::color) return;

    // Depth. Light culling and GTAO sample it on the compute queue, so it is
    // shared with it; copies seed the sea's depth from it.
    const gpu::TextureUses sampled_depth = Use::depth_target | Use::sampled;
    textures_.depth = make("depth", DEPTH_FORMAT, samples_, sampled_depth, gpu::Queues::graphics_and_compute);
    textures_.resolve_depth = msaa ? make("resolve_depth", DEPTH_FORMAT, gpu::Samples::x1, sampled_depth,
                                          gpu::Queues::graphics_and_compute)
                                   : gpu::Owned<gpu::Texture>{};

    // The sea's own depth, so it can hide itself. A Gerstner surface displaces
    // horizontally and folds through itself where the trains pinch hard, and a
    // crest then genuinely stands in front of the water behind it — but the
    // transparent pass binds the scene depth read-only, because a dozen
    // shaders sample that image meaning "the opaque scene" and water is not
    // part of it. So the sea gets a depth buffer of its own: one it may write,
    // holding the opaque scene with the water laid over it, seeded from the
    // scene depth each frame it is used.
    //
    // Same sample count as the target, because the pipelines that write it are
    // the target's. What asks how far away a pixel is *after* the sea is drawn
    // reads it as the surface depth: through the resolve under MSAA, and
    // directly at one sample.
    textures_.water_depth =
        make("water_depth", DEPTH_FORMAT, samples_, msaa ? gpu::TextureUses{Use::depth_target} : sampled_depth);
    textures_.resolve_water_depth = msaa ? make("resolve_water_depth", DEPTH_FORMAT, gpu::Samples::x1, sampled_depth)
                                         : gpu::Owned<gpu::Texture>{};

    // The surface targets written beside colour. Under MSAA only their
    // resolves are sampled. GTAO's blur reads the normals on the compute
    // queue, so those are shared with it.
    auto surface = [&](std::string_view what, gpu::Format format, gpu::Queues queues,
                       gpu::Owned<gpu::Texture>& image, gpu::Owned<gpu::Texture>& resolve) {
        image = make(what, format, samples_, msaa ? gpu::TextureUses{Use::color_target} : sampled_target, queues);
        resolve = msaa ? make("resolve_" + std::string(what), format, gpu::Samples::x1, sampled_target, queues)
                       : gpu::Owned<gpu::Texture>{};
    };
    surface("mr", MR_FORMAT, gpu::Queues::graphics, textures_.mr, textures_.resolve_mr);
    surface("normal", NORMAL_FORMAT, gpu::Queues::graphics_and_compute, textures_.normal, textures_.resolve_normal);
    surface("velocity", VELOCITY_FORMAT, gpu::Queues::graphics, textures_.velocity, textures_.resolve_velocity);

    // The copy transparents sample, bound through the global set to shaders
    // that may run before the copy is first made: it starts cleared.
    textures_.screen_color =
        make("screen_color", COLOR_FORMAT, gpu::Samples::x1, Use::sampled, gpu::Queues::graphics, black);
    textures_.distortion = make("distortion", DISTORTION_FORMAT, gpu::Samples::x1, sampled_target);
}

} // namespace fjell
