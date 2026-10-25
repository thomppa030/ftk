#pragma once

// Pictures the UI shows from the CPU: pixels made a texture, and SVG files
// drawn at exactly the pixels they cover.
//
//     auto mark = ftk::SvgPicture::load(brand / "fjell-mark-dark.svg");
//     ...in the UI:
//     const float scale = ftk::ui::current().root_scale();
//     ftk::ui::image(mark->texture(device, 92.0f, scale), mark->size(92.0f));

#include "ftk/base/result.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/texture.hpp"
#include "ftk/ui/core/types.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace ftk {

namespace gpu {
class Device;
}

/// A texture of `pixels`, RGBA8 in sRGB row after row, `width` × `height`,
/// ready for the frames sent from now on. `name` labels it in debuggers and
/// validation messages. None, with why logged, when the pixels don't fill the
/// size or the device cannot make it.
[[nodiscard]] gpu::Owned<gpu::Texture> upload_picture(gpu::Device& device, std::span<const uint8_t> pixels,
                                                      uint32_t width, uint32_t height, const std::string& name);

/// An SVG file as the UI shows it: drawn at exactly the pixels it covers when
/// first asked, again only when that changes (a window moved to a display of
/// another scale), its proportions the SVG's own.
class SvgPicture {
public:
    /// The SVG file at `path`, read and checked now; why not when it doesn't
    /// read or isn't an SVG with a size.
    [[nodiscard]] static Result<SvgPicture> load(const std::filesystem::path& path);
    /// `text`, an SVG file's contents, named `name` in debuggers; why not when
    /// it isn't an SVG with a size.
    [[nodiscard]] static Result<SvgPicture> from_text(std::string text, std::string name);

    /// Its size in UI units at `height` units tall.
    [[nodiscard]] glm::vec2 size(float height) const;
    /// The pixels it covers at `height` UI units tall on a display of `scale`
    /// pixels a unit: at least one each way, its width by its proportions.
    [[nodiscard]] glm::uvec2 pixels(float height, float scale) const;
    /// The picture at `height` UI units tall on a display of `scale` pixels a
    /// unit, as the current UI layer draws it. None without a UI layer (a
    /// headless test), or when it can't be drawn or made a texture, which is
    /// logged the once.
    [[nodiscard]] ui::Texture texture(gpu::Device& device, float height, float scale);

private:
    SvgPicture(std::string text, std::string name, float own_width, float own_height);

    std::string text_;
    std::string name_;
    // The size the SVG gives itself, in its own units.
    float own_width_;
    float own_height_;
    // The pixels the texture holds, or was last tried at.
    glm::uvec2 drawn_{0};
    gpu::Owned<gpu::Texture> texture_;
};

} // namespace ftk
