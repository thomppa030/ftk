#include "renderer/post_process_resources.hpp"

#include <algorithm>

namespace fjell {

PostProcessResources::PostProcessResources(VkDevice device, VmaAllocator allocator,
                                            uint32_t width, uint32_t height,
                                            const PostProcessConfig& config,
                                            const PassRefs& passes)
    : device_(device), allocator_(allocator), config_(config)
    , passes_storage_(std::make_unique<PassRefs>(passes)) {

    passes_ = passes_storage_.get();

    if (config_.hiz && width > 0 && height > 0) {
        hiz_images_.create(device_, allocator_,
                           passes_->hiz.image_set_layout(), passes_->hiz.sampler(),
                           width, height);
    }
    if (config_.gtao) {
        uint32_t half_w = std::max(1u, width / 2);
        uint32_t half_h = std::max(1u, height / 2);
        gtao_images_.create(device_, allocator_,
                            passes_->gtao.image_set_layout(), passes_->gtao.ao_sampler(),
                            half_w, half_h);
    }
    if (config_.bloom && width > 0 && height > 0) {
        bloom_images_.create(device_, allocator_,
                             passes_->bloom.single_tex_set_layout(),
                             passes_->bloom.composite_set_layout(),
                             passes_->bloom.bloom_sampler(),
                             width, height);
    }
    if (config_.ssr && width > 0 && height > 0) {
        ssr_images_.create(device_, allocator_,
                           passes_->ssr.trace_set_layout(),
                           passes_->ssr.composite_set_layout(),
                           passes_->ssr.nearest_sampler(),
                           width, height);
    }
    if (config_.volumetric_fog) {
        uint32_t vol_w = std::max(1u, width / VolumetricFogPass::SCREEN_DIVISOR);
        uint32_t vol_h = std::max(1u, height / VolumetricFogPass::SCREEN_DIVISOR);
        fog_images_.create(device_, allocator_,
                           passes_->fog.inject_set_layout(),
                           passes_->fog.temporal_set_layout(),
                           passes_->fog.integrate_set_layout(),
                           passes_->fog.composite_set_layout(),
                           passes_->fog.fog_sampler(),
                           vol_w, vol_h);
    }
    if (config_.tonemap && width > 0 && height > 0) {
        tonemap_images_.create(device_, allocator_,
                               passes_->tonemap.image_set_layout(),
                               width, height);
    }
    if (config_.ddgi && width > 0 && height > 0) {
        ddgi_images_.create(device_, allocator_,
                            passes_->ddgi.config(),
                            width, height);
    }
}

PostProcessResources::~PostProcessResources() {
    if (device_ == VK_NULL_HANDLE) return;
    hiz_images_.destroy(device_, allocator_);
    gtao_images_.destroy(device_, allocator_);
    bloom_images_.destroy(device_, allocator_);
    ssr_images_.destroy(device_, allocator_);
    fog_images_.destroy(device_, allocator_);
    tonemap_images_.destroy(device_, allocator_);
    ddgi_images_.destroy(device_, allocator_);
}

PostProcessResources::PostProcessResources(PostProcessResources&& other) noexcept
    : device_(other.device_), allocator_(other.allocator_)
    , config_(other.config_)
    , passes_storage_(std::move(other.passes_storage_))
    , hiz_images_(other.hiz_images_), gtao_images_(other.gtao_images_)
    , bloom_images_(other.bloom_images_), ssr_images_(other.ssr_images_)
    , fog_images_(other.fog_images_), tonemap_images_(other.tonemap_images_)
    , ddgi_images_(std::move(other.ddgi_images_)) {
    passes_ = passes_storage_.get();
    other.device_ = VK_NULL_HANDLE;
    other.hiz_images_ = {};
    other.gtao_images_ = {};
    other.bloom_images_ = {};
    other.ssr_images_ = {};
    other.fog_images_ = {};
    other.tonemap_images_ = {};
    other.ddgi_images_ = {};
}

PostProcessResources& PostProcessResources::operator=(PostProcessResources&& other) noexcept {
    if (this != &other) {
        if (device_ != VK_NULL_HANDLE) {
            hiz_images_.destroy(device_, allocator_);
            gtao_images_.destroy(device_, allocator_);
            bloom_images_.destroy(device_, allocator_);
            ssr_images_.destroy(device_, allocator_);
            fog_images_.destroy(device_, allocator_);
            tonemap_images_.destroy(device_, allocator_);
            ddgi_images_.destroy(device_, allocator_);
        }
        device_ = other.device_;
        allocator_ = other.allocator_;
        config_ = other.config_;
        passes_storage_ = std::move(other.passes_storage_);
        passes_ = passes_storage_.get();
        hiz_images_ = other.hiz_images_;
        gtao_images_ = other.gtao_images_;
        bloom_images_ = other.bloom_images_;
        ssr_images_ = other.ssr_images_;
        fog_images_ = other.fog_images_;
        tonemap_images_ = other.tonemap_images_;
        ddgi_images_ = std::move(other.ddgi_images_);
        other.device_ = VK_NULL_HANDLE;
        other.hiz_images_ = {};
        other.gtao_images_ = {};
        other.bloom_images_ = {};
        other.ssr_images_ = {};
        other.fog_images_ = {};
        other.tonemap_images_ = {};
        other.ddgi_images_ = {};
    }
    return *this;
}

void PostProcessResources::resize(uint32_t width, uint32_t height) {
    if (passes_ == nullptr || width == 0 || height == 0) return;

    if (config_.hiz) {
        hiz_images_.resize(device_, allocator_,
                           passes_->hiz.image_set_layout(), passes_->hiz.sampler(),
                           width, height);
    }
    if (config_.gtao) {
        gtao_images_.resize(device_, allocator_,
                            passes_->gtao.image_set_layout(), passes_->gtao.ao_sampler(),
                            width, height);
    }
    if (config_.bloom) {
        bloom_images_.resize(device_, allocator_,
                             passes_->bloom.single_tex_set_layout(),
                             passes_->bloom.composite_set_layout(),
                             passes_->bloom.bloom_sampler(),
                             width, height);
    }
    if (config_.ssr) {
        ssr_images_.resize(device_, allocator_,
                           passes_->ssr.trace_set_layout(),
                           passes_->ssr.composite_set_layout(),
                           passes_->ssr.nearest_sampler(),
                           width, height);
    }
    if (config_.volumetric_fog) {
        fog_images_.resize(device_, allocator_,
                           passes_->fog.inject_set_layout(),
                           passes_->fog.temporal_set_layout(),
                           passes_->fog.integrate_set_layout(),
                           passes_->fog.composite_set_layout(),
                           passes_->fog.fog_sampler(),
                           width, height);
    }
    if (config_.tonemap) {
        tonemap_images_.resize(device_, allocator_,
                               passes_->tonemap.image_set_layout(),
                               width, height);
    }
    if (config_.ddgi) {
        ddgi_images_.resize(device_, allocator_,
                            passes_->ddgi.config(),
                            width, height);
    }
}

void PostProcessResources::invalidate_imgui_textures() {
    gtao_images_.invalidate_imgui_texture();
    bloom_images_.invalidate_imgui_texture();
    ssr_images_.invalidate_imgui_texture();
    fog_images_.invalidate_imgui_texture();
    tonemap_images_.invalidate_imgui_texture();
    ddgi_images_.invalidate_imgui_texture();
}

} // namespace fjell
