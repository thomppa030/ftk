#include "renderer/scene_viewport.hpp"
#include "renderer/bindless.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "core/log.hpp"

namespace fjell {

SceneViewport::SceneViewport(Device& device, VkDevice vk_device, VmaAllocator allocator,
                             const SceneViewportConfig& config,
                             const PostProcessResources::PassRefs& pass_refs,
                             uint32_t reserved_bindless_index)
    : config_(config)
    , bindless_index_(reserved_bindless_index)
    , vk_device_(vk_device) {
    offscreen_ = std::make_unique<OffscreenPass>(device, allocator, config_.width, config_.height, config_.samples);

    pp_ = std::make_unique<PostProcessResources>(
        vk_device, allocator, config_.width, config_.height, config_.pp_config, pass_refs);

    output_sampler_ = vk_utils::create_linear_clamp_sampler(vk_device_);

    FJELL_GFX_DEBUG("SceneViewport '{}' created ({}x{}, bindless index {})",
                    config_.name, config_.width, config_.height, bindless_index_);
}

SceneViewport::~SceneViewport() {
    pp_.reset();
    offscreen_.reset();
    if (output_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(vk_device_, output_sampler_, nullptr);
    }
}

void SceneViewport::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    config_.width = width;
    config_.height = height;
    offscreen_->resize(width, height);
    pp_->resize(width, height);
    last_synced_view_ = VK_NULL_HANDLE; // force bindless update on next sync
}

VkImageView SceneViewport::output_view() const {
    if (!config_.expose_hdr && config_.pp_config.tonemap) {
        auto* tonemap = pp_->tonemap();
        if (tonemap && tonemap->view != VK_NULL_HANDLE) {
            return tonemap->view;
        }
    }
    return offscreen_->color_view();
}

void SceneViewport::sync_bindless(BindlessTextures& bindless) {
    VkImageView current = output_view();
    if (current == VK_NULL_HANDLE || current == last_synced_view_) return;

    bindless.update_entry(bindless_index_, current, output_sampler_);
    last_synced_view_ = current;
}

ViewportRenderRequest SceneViewport::build_request() const {
    ViewportRenderRequest req;
    req.target = offscreen_.get();
    req.view = view_.view;
    req.projection = view_.projection;
    req.camera_position = view_.camera_position;
    req.pp_resources = pp_.get();
    req.skip_shadows = config_.skip_shadows;
    req.flip_tonemap_y = true;
    req.viewport_graph = config_.viewport_graph;
    return req;
}

} // namespace fjell
