#include "renderer/scene_viewport.hpp"
#include "renderer/bindless.hpp"
#include "core/log.hpp"
// Through the include path: a quoted "gpu/device.hpp" finds the older
// renderer/gpu/device.hpp beside this file first.
#include <gpu/device.hpp>

namespace fjell {

SceneViewport::SceneViewport(Device& device, gpu::Device& gpu, VkDevice vk_device,
                             VmaAllocator allocator,
                             const SceneViewportConfig& config,
                             const PostProcessResources::PassRefs& pass_refs,
                             uint32_t reserved_bindless_index)
    : config_(config)
    , bindless_index_(reserved_bindless_index) {
    offscreen_ = std::make_unique<OffscreenPass>(device, gpu, allocator, config_.width,
                                                 config_.height, config_.samples);

    pp_ = std::make_unique<PostProcessResources>(
        gpu, vk_device, allocator, config_.width, config_.height, config_.pp_config, pass_refs);

    output_sampler_ = gpu.sampler({.address = gpu::Address::clamp});

    FJELL_GFX_DEBUG("SceneViewport '{}' created ({}x{}, bindless index {})",
                    config_.name, config_.width, config_.height, bindless_index_);
}

SceneViewport::~SceneViewport() {
    pp_.reset();
    offscreen_.reset();
}

void SceneViewport::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    config_.width = width;
    config_.height = height;
    offscreen_->resize(width, height);
    pp_->resize(width, height);
    last_synced_ = {}; // force bindless update on next sync
}

gpu::Texture SceneViewport::output_texture() const {
    if (!config_.expose_hdr && config_.pp_config.post) {
        auto* output = pp_->post();
        if (output && output->texture) return output->texture;
    }
    return offscreen_->color_texture();
}

void SceneViewport::sync_bindless(BindlessTextures& bindless) {
    const gpu::Texture current = output_texture();
    if (!current.valid() || current == last_synced_) return;

    bindless.update_entry(bindless_index_, current, output_sampler_);
    last_synced_ = current;
}

ViewportRenderRequest SceneViewport::build_request() const {
    ViewportRenderRequest req;
    req.target = offscreen_.get();
    req.view = view_.view;
    req.projection = view_.projection;
    req.camera_position = view_.camera_position;
    req.pp_resources = pp_.get();
    req.skip_shadows = config_.skip_shadows;
    req.viewport_graph = config_.viewport_graph;
    return req;
}

} // namespace fjell
