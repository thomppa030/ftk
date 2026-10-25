#include "ftk/app/picture.hpp"
#include "ftk/app/ui_layer.hpp"
#include "ftk/base/file_io.hpp"
#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/image/svg.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace ftk {

gpu::Owned<gpu::Texture> upload_picture(gpu::Device& device, std::span<const uint8_t> pixels, uint32_t width,
                                        uint32_t height, const std::string& name) {
    FTK_PROFILE_SCOPE_N("picture_upload");
    const size_t size = static_cast<size_t>(width) * height * 4;
    if (width == 0 || height == 0 || pixels.size() < size) {
        FTK_CORE_ERROR("Picture {} has {} bytes for {}x{} pixels", name, pixels.size(), width, height);
        return {};
    }
    // The pixels are sRGB; sampling through an sRGB format hands the UI light,
    // which it encodes back to the authored colour.
    auto made = device.create(gpu::TextureDesc{
        .format = gpu::Format::rgba8_srgb,
        .width = width,
        .height = height,
        .use = gpu::TextureUse::sampled,
        .name = name,
    });
    if (!made) {
        FTK_CORE_ERROR("Picture {}: {}", name, made.error());
        return {};
    }
    device.upload().to_texture(*made, {}, std::as_bytes(pixels.first(size)), {.after = gpu::Access::sampled_fragment});
    return std::move(*made);
}

SvgPicture::SvgPicture(std::string text, std::string name, float own_width, float own_height)
    : text_(std::move(text)), name_(std::move(name)), own_width_(own_width), own_height_(own_height) {}

Result<SvgPicture> SvgPicture::load(const std::filesystem::path& path) {
    auto text = read_text(path);
    if (!text) return std::unexpected(std::move(text).error());
    return from_text(std::move(*text), path.filename().string());
}

Result<SvgPicture> SvgPicture::from_text(std::string text, std::string name) {
    const auto own = image::svg_size(text);
    if (!own) return make_error(std::format("{}: {}", name, own.error()));
    return SvgPicture(std::move(text), std::move(name), own->width, own->height);
}

glm::vec2 SvgPicture::size(float height) const {
    return {height * own_width_ / own_height_, height};
}

glm::uvec2 SvgPicture::pixels(float height, float scale) const {
    const float tall = std::max(1.0f, std::round(height * scale));
    const float wide = std::max(1.0f, std::round(tall * own_width_ / own_height_));
    return {static_cast<uint32_t>(wide), static_cast<uint32_t>(tall)};
}

ui::Texture SvgPicture::texture(gpu::Device& device, float height, float scale) {
    const glm::uvec2 wanted = pixels(height, scale);
    if (wanted != drawn_) {
        drawn_ = wanted;
        texture_ = {};
        const auto drawn = image::rasterize_svg(text_, wanted.x, wanted.y);
        if (!drawn) {
            FTK_CORE_ERROR("{} isn't drawn at {}x{}: {}", name_, wanted.x, wanted.y, drawn.error());
            return {};
        }
        texture_ = upload_picture(device, drawn->texels, drawn->width, drawn->height, name_);
    }
    return texture_ ? ui_texture(texture_) : ui::Texture{};
}

} // namespace ftk
