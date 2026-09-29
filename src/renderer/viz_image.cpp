#include "renderer/viz_image.hpp"

#include "core/log.hpp"
#include "gpu/device.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/pass_builder.hpp"

#include <imgui_impl_vulkan.h>

namespace fjell {

VizImage::~VizImage() {
    cleanup();
}

void VizImage::init(gpu::Device& device, gpu::Format format, std::string name) {
    device_ = &device;
    format_ = format;
    name_ = std::move(name);
    sampler_ = device.sampler({.filter = gpu::Filter::linear,
                               .mip_filter = gpu::Filter::nearest,
                               .address = gpu::Address::clamp});
    create(64, 64);
}

void VizImage::cleanup() {
    invalidate_imgui_texture();
    texture_.reset();
    extent_ = {0, 0};
}

void VizImage::create(uint32_t width, uint32_t height) {
    auto made = device_->create(gpu::TextureDesc{
        .format = format_,
        .width = width,
        .height = height,
        .use = gpu::TextureUse::color_target | gpu::TextureUse::sampled,
        .name = name_,
    });
    if (!made) {
        FJELL_GFX_ERROR("{}", made.error());
        return;
    }
    texture_ = std::move(*made);
    extent_ = {width, height};
    has_rendered_ = false;
}

void VizImage::resize(uint32_t width, uint32_t height) {
    if (device_ == nullptr || (texture_ && extent_.x == width && extent_.y == height)) return;
    // The editor's view of the old image may still be in a frame's draw
    // data; it goes when that frame is done.
    if (imgui_descriptor_ != VK_NULL_HANDLE) {
        gpu::vulkan::defer(*device_, [set = imgui_descriptor_] { ImGui_ImplVulkan_RemoveTexture(set); });
        imgui_descriptor_ = VK_NULL_HANDLE;
    }
    create(width, height);
}

void VizImage::collect_exports(DeclareContext& ctx) const {
    if (!texture_) return;
    ctx.export_texture(name_, texture_,
                       {.resting = gpu::Access::sampled_fragment, .unwritten = !has_rendered_});
}

bool VizImage::declare(PassBuilder& builder, const DeclareContext& ctx) const {
    auto image = builder.import_named_optional(ctx, name_);
    if (!image.valid()) return false;
    builder.write(image, gpu::Access::color_attachment);
    // ImGui reads the image after the graph, so nothing in it reads what the
    // pass writes, and the cull walk would drop the pass.
    builder.has_side_effects();
    return true;
}

gpu::RenderTargets VizImage::targets(const gpu::Clear& clear) const {
    return {.color = {{.view = texture_, .load = gpu::Load::clear, .clear = clear}}};
}

VkDescriptorSet VizImage::imgui_texture() {
    if (!has_rendered_) return VK_NULL_HANDLE;
    if (imgui_descriptor_ == VK_NULL_HANDLE && texture_) {
        imgui_descriptor_ = ImGui_ImplVulkan_AddTexture(
            gpu::vulkan::native_sampler(*device_, sampler_), gpu::vulkan::native_view(*device_, texture_),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor_;
}

void VizImage::invalidate_imgui_texture() {
    if (imgui_descriptor_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor_);
        imgui_descriptor_ = VK_NULL_HANDLE;
    }
}

VkImageView VizImage::view() const {
    return texture_ ? gpu::vulkan::native_view(*device_, texture_) : VK_NULL_HANDLE;
}

VkImage VizImage::image() const {
    return texture_ ? gpu::vulkan::native_image(*device_, texture_) : VK_NULL_HANDLE;
}

VkSampler VizImage::sampler() const {
    return gpu::vulkan::native_sampler(*device_, sampler_);
}

} // namespace fjell
