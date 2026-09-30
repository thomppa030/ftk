#include "ui/icon_cache.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "ui/imgui_layer.hpp"

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <algorithm>
#include <filesystem>
#include <vector>

namespace fjell {

namespace fs = std::filesystem;

namespace {

/// An image as four bytes a pixel, row after row.
struct Picture {
    int width{0};
    int height{0};
    std::vector<uint8_t> rgba;
};

fs::path thumbnail_cache_path(const fs::path& source) {
    return source.parent_path() / ".fjcache" / (source.stem().string() + ".thumb.png");
}

/// Whether `cache` holds a thumbnail at least as new as `source`.
bool cache_current(const fs::path& source, const fs::path& cache) {
    std::error_code ec;
    const auto cache_time = fs::last_write_time(cache, ec);
    if (ec) return false;
    const auto source_time = fs::last_write_time(source, ec);
    return !ec && cache_time >= source_time;
}

/// The image file at `path` as RGBA; empty when it does not read.
Picture read_rgba(const fs::path& path) {
    Picture picture;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &picture.width, &picture.height, &channels, 4);
    if (pixels == nullptr) return {};
    picture.rgba.assign(pixels, pixels + static_cast<size_t>(picture.width) * picture.height * 4);
    stbi_image_free(pixels);
    return picture;
}

/// `source` fitted into the thumbnail square and written to `cache`; empty
/// when it does not read.
Picture make_thumbnail(const fs::path& source, const fs::path& cache) {
    Picture full = read_rgba(source);
    if (full.rgba.empty()) return {};

    constexpr int SIZE = IconCache::THUMBNAIL_SIZE;
    Picture thumb;
    if (full.width > SIZE || full.height > SIZE) {
        const float scale = static_cast<float>(SIZE) / static_cast<float>(std::max(full.width, full.height));
        thumb.width = std::max(1, static_cast<int>(static_cast<float>(full.width) * scale));
        thumb.height = std::max(1, static_cast<int>(static_cast<float>(full.height) * scale));
        thumb.rgba.resize(static_cast<size_t>(thumb.width) * thumb.height * 4);
        stbir_resize_uint8_linear(full.rgba.data(), full.width, full.height, 0,
                                  thumb.rgba.data(), thumb.width, thumb.height, 0, STBIR_RGBA);
    } else {
        thumb = std::move(full);
    }

    std::error_code ec;
    fs::create_directories(cache.parent_path(), ec);
    if (!ec) {
        stbi_write_png(cache.string().c_str(), thumb.width, thumb.height, 4, thumb.rgba.data(), thumb.width * 4);
    }
    return thumb;
}

/// `source`'s thumbnail: its current `.fjcache` copy, else one made and
/// written there.
Picture load_thumbnail(const fs::path& source) {
    const fs::path cache = thumbnail_cache_path(source);
    if (cache_current(source, cache)) {
        Picture cached = read_rgba(cache);
        if (!cached.rgba.empty()) return cached;
    }
    return make_thumbnail(source, cache);
}

} // namespace

IconCache::IconCache(gpu::Device& device, const std::string& icons_dir)
    : device_{device},
      pixel_sampler_{device.sampler({.filter = gpu::Filter::nearest,
                                     .mip_filter = gpu::Filter::nearest,
                                     .address = gpu::Address::clamp})} {
    if (!fs::is_directory(icons_dir)) {
        FJELL_CORE_WARN("Icons directory not found: {}", icons_dir);
        return;
    }

    for (const auto& entry : fs::directory_iterator(icons_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".png") continue;

        auto filename = entry.path().stem().string();
        if (filename.starts_with("icon_")) {
            filename = filename.substr(5);
        }
        load_icon(filename, entry.path().string());
    }

    FJELL_CORE_INFO("Loaded {} icons", icons_.size());
}

IconCache::~IconCache() = default;

ImTextureID IconCache::icon(const std::string& name) const {
    const auto it = icons_.find(name);
    return it != icons_.end() ? imgui_texture(it->second) : ImTextureID{};
}

gpu::Owned<gpu::Texture> IconCache::upload_rgba(std::span<const uint8_t> pixels, uint32_t width, uint32_t height,
                                                const std::string& name) {
    FJELL_PROFILE_SCOPE_N("icon_upload");
    const size_t size = static_cast<size_t>(width) * height * 4;
    if (width == 0 || height == 0 || pixels.size() < size) {
        FJELL_CORE_ERROR("Icon cache: {} has {} bytes for {}x{} pixels", name, pixels.size(), width, height);
        return {};
    }
    // PNG pixels are sRGB; sampling through an sRGB format hands ImGui
    // linear light, which the swapchain encodes back to the authored colour.
    auto made = device_.create(gpu::TextureDesc{
        .format = gpu::Format::rgba8_srgb,
        .width = width,
        .height = height,
        .use = gpu::TextureUse::sampled,
        .name = name,
    });
    if (!made) {
        FJELL_CORE_ERROR("Icon cache: {}: {}", name, made.error());
        return {};
    }
    device_.upload().to_texture(*made, {}, std::as_bytes(pixels.first(size)),
                                {.after = gpu::Access::sampled_fragment});
    return std::move(*made);
}

void IconCache::load_icon(const std::string& name, const std::string& path) {
    const Picture picture = read_rgba(path);
    if (picture.rgba.empty()) {
        FJELL_CORE_WARN("Failed to load icon: {}", path);
        return;
    }
    icons_[name] = upload_rgba(picture.rgba, static_cast<uint32_t>(picture.width),
                               static_cast<uint32_t>(picture.height), "icon " + name);
}

ImTextureID IconCache::thumbnail(const std::string& path) {
    auto it = thumbnails_.find(path);
    if (it == thumbnails_.end()) {
        const Picture picture = load_thumbnail(path);
        if (picture.rgba.empty()) return {};
        it = thumbnails_
                 .emplace(path, upload_rgba(picture.rgba, static_cast<uint32_t>(picture.width),
                                            static_cast<uint32_t>(picture.height), "thumbnail " + path))
                 .first;
    }
    return it->second ? imgui_texture(it->second) : ImTextureID{};
}

ImTextureID IconCache::thumbnail_cached(const std::string& path) const {
    const auto it = thumbnails_.find(path);
    return it != thumbnails_.end() && it->second ? imgui_texture(it->second) : ImTextureID{};
}

void IconCache::ensure_thumbnail_cache(const std::string& path) {
    const fs::path source(path);
    const fs::path cache = thumbnail_cache_path(source);
    if (!cache_current(source, cache)) (void)make_thumbnail(source, cache);
}

void IconCache::clear_thumbnails() {
    // What was handed out this frame stays until the frames drawing it are
    // done; the textures' release waits for them.
    thumbnails_.clear();
}

} // namespace fjell
