#include "ftk/app/app_identity.hpp"
#include "ftk/base/file_io.hpp"
#include "ftk/gpu/window.hpp"
#include "ftk/image/svg.hpp"

#include <format>

namespace ftk {

Result<std::vector<image::Rgba8>> draw_app_icon(std::string_view svg) {
    std::vector<image::Rgba8> sizes;
    sizes.reserve(APP_ICON_SIZES.size());
    for (const uint32_t size : APP_ICON_SIZES) {
        auto drawn = image::rasterize_svg(svg, 0, size);
        if (!drawn) return std::unexpected(std::move(drawn).error());
        sizes.push_back(std::move(*drawn));
    }
    return sizes;
}

Result<> set_app_identity(const AppIdentity& identity) {
    Window::set_app_id(identity.id, identity.name);
    const auto svg = read_text(identity.icon);
    if (!svg) return make_error(std::format("The icon: {}", svg.error()));
    const auto sizes = draw_app_icon(*svg);
    if (!sizes) return make_error(std::format("The icon {}: {}", identity.icon.string(), sizes.error()));
    std::vector<WindowIcon> icons;
    icons.reserve(sizes->size());
    for (const image::Rgba8& size : *sizes) {
        icons.push_back({.width = size.width, .height = size.height, .rgba = size.texels});
    }
    Window::set_icon(icons);
    return {};
}

} // namespace ftk
