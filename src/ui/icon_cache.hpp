#pragma once

#include "gpu/owned.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fjell {

class ThreadPool;
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
    /// `icon_` prefix. Thumbnails are decoded on `pool`.
    IconCache(gpu::Device& device, ThreadPool& pool, const std::string& icons_dir);
    ~IconCache();

    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;
    IconCache(IconCache&&) = delete;
    IconCache& operator=(IconCache&&) = delete;

    /// A named icon (e.g. "folder", "mesh") as ImGui draws it; none when
    /// there is no such icon.
    [[nodiscard]] ImTextureID icon(const std::string& name) const;

    /// The thumbnail of the image file at `path` as ImGui draws it. The first
    /// ask decodes it on the pool, from its `.fjcache` copy when that is
    /// newer than the file; none until it has arrived, or when the file does
    /// not read.
    [[nodiscard]] ImTextureID thumbnail(const std::string& path);

    /// Forgets every thumbnail, and drops what is still being decoded.
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

    /// Thumbnails decoded on the pool, waiting to be uploaded. Shared with the
    /// decodes, which may finish after the cache is gone.
    struct Inbox;

    void load_icon(const std::string& name, const std::string& path);
    /// Uploads the thumbnails decoded since the last call.
    void take_decoded();

    gpu::Device& device_;
    ThreadPool& pool_;
    gpu::Sampler pixel_sampler_;

    std::unordered_map<std::string, gpu::Owned<gpu::Texture>> icons_;
    /// Every thumbnail that has arrived, by path; an empty texture for a
    /// file that did not read, so it is not asked for again.
    std::unordered_map<std::string, gpu::Owned<gpu::Texture>> thumbnails_;
    std::unordered_set<std::string> decoding_;
    /// Moves on at every clear, so a decode started before one is dropped.
    uint64_t generation_{0};
    std::shared_ptr<Inbox> inbox_;
};

} // namespace fjell
