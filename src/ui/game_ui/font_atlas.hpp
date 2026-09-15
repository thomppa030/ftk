#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace fjell {

class Texture;
class BindlessTextures;
class UploadContext;

struct GlyphInfo {
    float uv_x0, uv_y0;   // top-left UV in atlas
    float uv_x1, uv_y1;   // bottom-right UV in atlas
    float quad_x;          // quad left offset from cursor (em-normalized, includes SDF padding)
    float quad_y;          // quad top offset from baseline (em-normalized, includes SDF padding)
    float quad_w;          // full quad width (em-normalized, includes SDF padding)
    float quad_h;          // full quad height (em-normalized, includes SDF padding)
    float advance;         // horizontal advance to next glyph (em-normalized)
};

struct FontMetrics {
    float ascender;        // distance from baseline to top (em-normalized)
    float descender;       // distance from baseline to bottom (em-normalized, negative)
    float line_height;     // recommended line spacing (em-normalized)
};

/// The MSDF atlas of one `.fjfont` recipe on the GPU, registered in the
/// bindless system. The atlas itself is baked offline (see fjfont.hpp): this
/// loads the cached bake beside the recipe, rebuilding it first when the
/// recipe or its font file is newer.
///
/// Glyph metrics are stored in em-normalized space — multiply by font_size
/// to get pixel coordinates at render time.
///
/// The atlas stores MSDF data in RGB channels (UNORM, no gamma, no mipmaps).
/// The fragment shader must apply the median-of-three + screenPxRange formula.
class FontAtlas {
public:
    FontAtlas() = default;
    ~FontAtlas();

    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;
    FontAtlas(FontAtlas&&) = delete;
    FontAtlas& operator=(FontAtlas&&) = delete;

    /// Loads the recipe at `recipe_path` and uploads its atlas. Failures are
    /// logged and leave the atlas invalid; text then draws nothing.
    void create(const std::string& recipe_path,
                VkDevice device, VmaAllocator allocator,
                VkPhysicalDevice physical_device,
                UploadContext& upload,
                BindlessTextures& bindless);

    void destroy();

    [[nodiscard]] const GlyphInfo* glyph(uint32_t codepoint) const;
    [[nodiscard]] float kerning(uint32_t left, uint32_t right) const;
    [[nodiscard]] const FontMetrics& metrics() const { return metrics_; }
    [[nodiscard]] uint32_t atlas_bindless_index() const { return atlas_index_; }
    [[nodiscard]] uint32_t atlas_width() const { return atlas_width_; }
    [[nodiscard]] uint32_t atlas_height() const { return atlas_height_; }
    [[nodiscard]] float pixel_range() const { return pixel_range_; }
    [[nodiscard]] bool valid() const { return atlas_texture_ != nullptr; }

private:
    struct PairHash {
        std::size_t operator()(std::pair<uint32_t, uint32_t> p) const {
            return std::hash<uint64_t>{}(
                (static_cast<uint64_t>(p.first) << 32) | p.second);
        }
    };

    std::unordered_map<uint32_t, GlyphInfo> glyphs_;
    std::unordered_map<std::pair<uint32_t, uint32_t>, float, PairHash> kerning_;
    FontMetrics metrics_{};
    std::unique_ptr<Texture> atlas_texture_;
    uint32_t atlas_index_{0};
    uint32_t atlas_width_{0};
    uint32_t atlas_height_{0};
    float pixel_range_{0.0f};
};

} // namespace fjell
