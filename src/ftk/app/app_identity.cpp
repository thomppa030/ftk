#include "ftk/app/app_identity.hpp"
#include "ftk/base/file_io.hpp"
#include "ftk/gpu/window.hpp"
#include "ftk/image/ico.hpp"

#include <format>
#include <vector>

namespace ftk {

Result<> set_app_identity(const AppIdentity& identity) {
    Window::set_app_id(identity.id, identity.name);
    const auto svg = read_text(identity.icon);
    if (!svg) return make_error(std::format("The icon: {}", svg.error()));
    const auto sizes = image::rasterize_icon(*svg);
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
