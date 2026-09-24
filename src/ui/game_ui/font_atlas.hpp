#pragma once

#include "core/result.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace fjell {

struct FjFontRecipe;

class Texture;
class BindlessTextures;
class DeferredDeleter;
class UploadContext;

/// What an atlas needs to reach the GPU. Kept by the atlas so it can
/// rebuild itself in place.
struct FontAtlasGpu {
    VkDevice device{VK_NULL_HANDLE};
    VmaAllocator allocator{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    UploadContext* upload{nullptr};
    BindlessTextures* bindless{nullptr};
    DeferredDeleter* deleter{nullptr};
};

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

/// Everything a recipe's atlas is before it reaches the GPU: the glyph and
/// kerning tables, the metrics and the pixels. Produced by
/// FontAtlas::load_data on any thread, since baking a large charset takes
/// long enough to stall a frame; adopted on the main thread.
struct FontAtlasData {
    std::unordered_map<uint32_t, GlyphInfo> glyphs;
    std::vector<std::pair<std::pair<uint32_t, uint32_t>, float>> kerning;
    FontMetrics metrics{};
    float pixel_range{0.0f};
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> pixels;
    std::string source_path;
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
    // Out of line: Texture is only forward-declared here.
    FontAtlas();
    ~FontAtlas();

    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;
    FontAtlas(FontAtlas&&) = delete;
    FontAtlas& operator=(FontAtlas&&) = delete;

    /// Reads a recipe and its cached atlas, baking the atlas first when the
    /// cache is stale. CPU only, safe on a worker thread; problems are
    /// logged and returned.
    [[nodiscard]] static Result<FontAtlasData> load_data(const std::string& recipe_path);

    /// Bakes `recipe` as it is, not as its file holds it, and writes no
    /// cache: an editor's unsaved recipe, previewed. `recipe_path` places
    /// the font file the recipe names. CPU only, safe on a worker thread.
    [[nodiscard]] static Result<FontAtlasData> bake_data(const std::string& recipe_path,
                                                         const FjFontRecipe& recipe);

    /// Takes the loaded data to the GPU. Main thread, between frames.
    void create(const std::string& recipe_path, const FontAtlasGpu& gpu, FontAtlasData data);

    /// Swaps freshly loaded data into the same bindless slot, so every glyph
    /// already recorded with this atlas's index keeps drawing. The old
    /// texture is retired through the deferred deleter, never freed under a
    /// frame in flight.
    void reload(FontAtlasData data);

    void destroy();

    [[nodiscard]] const std::string& recipe_path() const { return recipe_path_; }

    /// The atlas as an ImGui image, for an editor to show. Created on first
    /// use; a reload replaces it once the frames still drawing the old one
    /// are done.
    [[nodiscard]] VkDescriptorSet imgui_texture();
    /// Gives the ImGui image back. Must run while the ImGui backend is still
    /// up, which is before this atlas is destroyed at shutdown.
    void release_imgui_texture();
    /// The font file the recipe names, resolved; empty until loaded.
    [[nodiscard]] const std::string& source_path() const { return source_path_; }

    [[nodiscard]] const GlyphInfo* glyph(uint32_t codepoint) const;
    [[nodiscard]] float kerning(uint32_t left, uint32_t right) const;
    [[nodiscard]] const FontMetrics& metrics() const { return metrics_; }
    [[nodiscard]] uint32_t atlas_bindless_index() const { return atlas_index_; }
    [[nodiscard]] uint32_t atlas_width() const { return atlas_width_; }
    [[nodiscard]] size_t glyph_count() const { return glyphs_.size(); }
    [[nodiscard]] uint32_t atlas_height() const { return atlas_height_; }
    [[nodiscard]] float pixel_range() const { return pixel_range_; }
    [[nodiscard]] bool valid() const { return atlas_texture_ != nullptr; }
    /// Moves on every reload: whatever was drawn from the atlas before is
    /// out of date once it has.
    [[nodiscard]] uint32_t generation() const { return generation_; }

private:
    struct PairHash {
        std::size_t operator()(std::pair<uint32_t, uint32_t> p) const {
            return std::hash<uint64_t>{}(
                (static_cast<uint64_t>(p.first) << 32) | p.second);
        }
    };

    /// Installs the tables of `data`; the texture is the caller's business.
    void adopt(FontAtlasData& data);

    FontAtlasGpu gpu_{};
    std::string recipe_path_;
    std::string source_path_;
    std::unordered_map<uint32_t, GlyphInfo> glyphs_;
    std::unordered_map<std::pair<uint32_t, uint32_t>, float, PairHash> kerning_;
    FontMetrics metrics_{};
    std::unique_ptr<Texture> atlas_texture_;
    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};
    uint32_t atlas_index_{0};
    uint32_t atlas_width_{0};
    uint32_t atlas_height_{0};
    float pixel_range_{0.0f};
    uint32_t generation_{0};
};

} // namespace fjell
