#pragma once

#include "gpu/owned.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"

#include <imgui.h>

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

namespace fjell {

namespace gpu {
class Device;
}

/// The editor's pictures read from image files: its icons, and small
/// thumbnails of textures for the content browser and asset fields. What it
/// hands out is drawn through the current ImGui context's layer.
class IconCache {
public:
    /// Thumbnails fit in a square this many pixels a side.
    static constexpr int THUMBNAIL_SIZE = 80;

    /// Loads every PNG in `icons_dir` as an icon named by its file, less an
    /// `icon_` prefix.
    IconCache(gpu::Device& device, const std::string& icons_dir);
    ~IconCache();

    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;
    IconCache(IconCache&&) = delete;
    IconCache& operator=(IconCache&&) = delete;

    /// A named icon (e.g. "folder", "mesh") as ImGui draws it; none when
    /// there is no such icon.
    [[nodiscard]] ImTextureID icon(const std::string& name) const;

    /// The thumbnail of the image file at `path` as ImGui draws it, decoded
    /// on the first ask, from its `.fjcache` copy when that is newer than the
    /// file; none when the file does not read.
    [[nodiscard]] ImTextureID thumbnail(const std::string& path);

    /// The thumbnail if it has been decoded, without decoding it.
    [[nodiscard]] ImTextureID thumbnail_cached(const std::string& path) const;

    /// Forgets every thumbnail.
    void clear_thumbnails();

    /// Writes the `.fjcache` thumbnail of the image file at `path`, unless a
    /// current one is there. No GPU work; safe from any thread.
    static void ensure_thumbnail_cache(const std::string& path);

    /// A texture of `pixels`, RGBA8 in sRGB row after row, `width` × `height`,
    /// ready for the frames sent from now on. `name` labels it in debuggers
    /// and validation messages. None when the device cannot make it.
    [[nodiscard]] gpu::Owned<gpu::Texture> upload_rgba(std::span<const uint8_t> pixels, uint32_t width,
                                                       uint32_t height, const std::string& name);

    /// Nearest filtering, clamped: for showing a picture enlarged pixel by
    /// pixel.
    [[nodiscard]] gpu::Sampler pixel_sampler() const { return pixel_sampler_; }

    /// Global instance — set once at engine init, used by all panels.
    [[nodiscard]] static IconCache* instance() { return s_instance; }
    static void set_instance(IconCache* cache) { s_instance = cache; }

private:
    static inline IconCache* s_instance{nullptr};

    void load_icon(const std::string& name, const std::string& path);

    gpu::Device& device_;
    gpu::Sampler pixel_sampler_;

    std::unordered_map<std::string, gpu::Owned<gpu::Texture>> icons_;
    std::unordered_map<std::string, gpu::Owned<gpu::Texture>> thumbnails_;
};

} // namespace fjell
