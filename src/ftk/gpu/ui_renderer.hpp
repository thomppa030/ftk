#pragma once

// Draws what the UI core recorded for a window (ftk/ui/core/draw_list.hpp)
// with the GPU interface (decision 16 of the UI core plan). Each shape is
// one instance of a quad, read from the frame's memory, whose fragment
// shader works out the shape's distance; one draw covers every shape up to
// the next change of pictures.
//
// The UI is drawn into a canvas of its own whose 8-bit texels hold sRGB
// values, so colours mix as sRGB values, as the desktop and browsers mix
// them and as the design sheets were drawn. A last pass copies the canvas
// into the target, which the window's sRGB swapchain stores unchanged.
//
//     auto ui = gpu::UiRenderer::create(device, swapchain.format());
//     const ui::Texture preview = (*ui)->texture(preview_view);
//     ...
//     (*ui)->render(cmd, image->texture, {width, height}, out.roots[0], context.text().atlas(), background);

#include "ftk/base/result.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/format.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/texture.hpp"
#include "ftk/text/atlas.hpp"
#include "ftk/ui/core/context.hpp"

#include <glm/vec2.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace ftk::gpu {

class CommandList;

class UiRenderer {
public:
    /// What draws the UI into targets of `target_format`; its shaders are
    /// compiled into the library.
    /// @return the renderer, or why a pipeline or a texture could not be made
    [[nodiscard]] static Result<std::unique_ptr<UiRenderer>> create(Device& device, Format target_format);

    ~UiRenderer();
    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;
    UiRenderer(UiRenderer&&) = delete;
    UiRenderer& operator=(UiRenderer&&) = delete;

    /// A picture the UI can show: `view`, sampled in the fragment stage,
    /// whose texels the shader takes as sRGB values (an 8-bit UNORM
    /// texture holding them). The texture must outlive the handle's use;
    /// forget() lets the handle go.
    [[nodiscard]] ui::Texture texture(const TextureView& view);
    void forget(ui::Texture texture);

    /// Draws `root`, as the core recorded it, into `target`, `size` pixels,
    /// which is in `Access::color_attachment` and is left there. The regions
    /// of `atlas` written since the last frame are uploaded first.
    /// `background` is what nothing covers.
    void render(CommandList& cmd, const TextureView& target, glm::uvec2 size, const ui::core::RootOutput& root,
                text::GlyphAtlas& atlas, ui::Color background);

private:
    explicit UiRenderer(Device& device) : device_(&device) {}

    // The atlas's pages on the GPU, made as the atlas grows, and its new
    // regions written into them.
    [[nodiscard]] Result<> upload_atlas(text::GlyphAtlas& atlas);
    // The canvas, made again when the target's size changes.
    [[nodiscard]] Result<> fit_canvas(glm::uvec2 size);

    Device* device_;
    Owned<GraphicsPipeline> draw_;
    Owned<GraphicsPipeline> copy_;
    Sampler linear_{};
    Sampler nearest_{};
    // What a page or a picture slot holds when nothing is in it.
    Owned<Texture> blank_;
    std::vector<Owned<Texture>> pages_;
    Owned<Texture> canvas_;
    glm::uvec2 canvas_size_{0, 0};
    // Whether the canvas has been drawn and read before, for its transition.
    bool canvas_used_{false};
    std::unordered_map<uint64_t, TextureView> pictures_;
    uint64_t next_picture_{1};
    // This frame's shapes, with the pictures they name renumbered per draw.
    std::vector<ui::core::Primitive> primitives_;
    std::vector<uint8_t> region_;
};

} // namespace ftk::gpu
