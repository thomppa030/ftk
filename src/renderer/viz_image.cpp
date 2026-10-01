#include "renderer/viz_image.hpp"

#include "ftk/base/log.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/declare_context.hpp"

namespace fjell {

void VizImage::init(ftk::gpu::Device& device, ftk::gpu::Format format, std::string name) {
    device_ = &device;
    format_ = format;
    name_ = std::move(name);
    create(64, 64);
}

void VizImage::cleanup() {
    texture_.reset();
    extent_ = {0, 0};
}

void VizImage::create(uint32_t width, uint32_t height) {
    auto made = device_->create(ftk::gpu::TextureDesc{
        .format = format_,
        .width = width,
        .height = height,
        .use = ftk::gpu::TextureUse::color_target | ftk::gpu::TextureUse::sampled,
        .name = name_,
    });
    if (!made) {
        FTK_GFX_ERROR("{}", made.error());
        return;
    }
    texture_ = std::move(*made);
    extent_ = {width, height};
    has_rendered_ = false;
}

void VizImage::resize(uint32_t width, uint32_t height) {
    if (device_ == nullptr || (texture_ && extent_.x == width && extent_.y == height)) return;
    create(width, height);
}

void VizImage::collect_exports(DeclareContext& ctx) const {
    if (!texture_) return;
    ctx.export_texture(name_, texture_,
                       {.resting = ftk::gpu::Access::sampled_fragment, .unwritten = !has_rendered_});
}

bool VizImage::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) const {
    auto image = builder.import_named_optional(ctx, name_);
    if (!image.valid()) return false;
    builder.write(image, ftk::gpu::Access::color_attachment);
    // ImGui reads the image after the graph, so nothing in it reads what the
    // pass writes, and the cull walk would drop the pass.
    builder.has_side_effects();
    return true;
}

ftk::gpu::RenderTargets VizImage::targets(const ftk::gpu::Clear& clear) const {
    return {.color = {{.view = texture_, .load = ftk::gpu::Load::clear, .clear = clear}}};
}

ftk::gpu::RenderTargets VizImage::targets() const {
    return {.color = {{.view = texture_, .load = ftk::gpu::Load::discard}}};
}

} // namespace fjell
