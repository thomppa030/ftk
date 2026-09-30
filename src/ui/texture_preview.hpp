#pragma once

#include "gpu/owned.hpp"
#include "gpu/texture.hpp"
#include "ui/kit/canvas.hpp"
#include "ui/texture_channels.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

class IconCache;

/// A texture on its own (sheet 33, T1), opened from the content browser:
/// the picture as a canvas over a checkerboard, so its alpha reads (the
/// wheel zooms at the cursor, a middle drag pans, F fits, 1 shows it pixel
/// for pixel); pills over it for the channels and the zoom; the pixel under
/// the mouse read out; and a line under it naming the file, its size, its
/// channels, how large it is on disk and its folder.
class TexturePreview {
public:
    TexturePreview() = default;
    TexturePreview(const TexturePreview&) = delete;
    TexturePreview& operator=(const TexturePreview&) = delete;
    TexturePreview(TexturePreview&&) = delete;
    TexturePreview& operator=(TexturePreview&&) = delete;
    ~TexturePreview() = default;

    /// The cache that uploads the picture. Set before open().
    void init(IconCache* icons) { icons_ = icons; }

    /// Shows the image at `path`, read at the next update().
    void open(std::string path);

    /// Reads a newly opened file, rebuilds the picture after the channels
    /// changed, and lets go of it once the panel is closed. Call once a
    /// frame before anything draws.
    void update();

    void draw();

    /// Lets go of the picture now.
    void shutdown();

private:
    void load();
    void upload();
    void release();
    void draw_canvas(ImVec2 origin, ImVec2 size);
    void draw_info();
    void fit();
    void pixel_for_pixel();

    IconCache* icons_{nullptr};
    bool open_{false};
    std::string path_;
    std::string loaded_path_;
    bool failed_{false};

    std::vector<uint8_t> pixels_;  // as read, four bytes a pixel
    int width_{0};
    int height_{0};
    int file_channels_{0};
    std::uintmax_t file_size_{0};

    ChannelMask mask_{};
    ChannelMask uploaded_mask_{};
    gpu::Owned<gpu::Texture> picture_;

    ui::CanvasView view_{{.zoom = ui::CanvasZoom::Uniform, .min_scale = 1.0f / 64.0f, .max_scale = 64.0f}};
    bool framed_{false};
};

} // namespace fjell
