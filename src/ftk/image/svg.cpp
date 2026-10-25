#include "ftk/image/svg.hpp"

#include <lunasvg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>

namespace ftk::image {

namespace {

// One side from the other by the proportions `along` : `across`, at least a pixel.
uint32_t in_proportion(uint32_t other, float along, float across) {
    return static_cast<uint32_t>(std::max(1L, std::lround(static_cast<float>(other) * along / across)));
}

// The document `text` holds, with a size of its own; why not.
Result<std::unique_ptr<lunasvg::Document>> sized_document(std::string_view text) {
    std::unique_ptr<lunasvg::Document> document = lunasvg::Document::loadFromData(text.data(), text.size());
    if (!document) return make_error("Not an SVG file");
    if (!(document->width() > 0.0f) || !(document->height() > 0.0f)) {
        return make_error("The SVG has no size: no width and height, and no viewBox");
    }
    return document;
}

} // namespace

Result<SvgSize> svg_size(std::string_view text) {
    const auto document = sized_document(text);
    if (!document) return std::unexpected(document.error());
    return SvgSize{.width = (*document)->width(), .height = (*document)->height()};
}

Result<Rgba8> rasterize_svg(std::string_view text, uint32_t width, uint32_t height) {
    if (width == 0 && height == 0) return make_error("An SVG is drawn at a width or a height, and both are 0");
    if (width > SVG_LARGEST || height > SVG_LARGEST) {
        return make_error(std::format("{} × {} pixels is larger than an SVG is drawn at ({} on a side)", width, height,
                                      SVG_LARGEST));
    }
    auto sized = sized_document(text);
    if (!sized) return std::unexpected(sized.error());
    const std::unique_ptr<lunasvg::Document> document = std::move(*sized);
    const float own_width = document->width();
    const float own_height = document->height();
    if (width == 0) width = in_proportion(height, own_width, own_height);
    if (height == 0) height = in_proportion(width, own_height, own_width);
    if (width > SVG_LARGEST || height > SVG_LARGEST) {
        return make_error(std::format("{} × {} pixels, by the SVG's proportions, is larger than an SVG is drawn at "
                                      "({} on a side)",
                                      width, height, SVG_LARGEST));
    }

    lunasvg::Bitmap bitmap = document->renderToBitmap(static_cast<int>(width), static_cast<int>(height));
    if (bitmap.isNull()) return make_error(std::format("The SVG could not be drawn at {} × {} pixels", width, height));
    bitmap.convertToRGBA();
    Rgba8 picture{.texels = std::vector<uint8_t>(static_cast<std::size_t>(width) * height * 4), .width = width,
                  .height = height};
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(picture.texels.data() + y * row, bitmap.data() + static_cast<std::size_t>(y) * bitmap.stride(),
                    row);
    }
    return picture;
}

} // namespace ftk::image
