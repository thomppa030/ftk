#include "ftk/text/atlas.hpp"

#include "ftk/text/detail/face_impl.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ftk::text {

namespace {

// Empty pixels kept around each glyph, so that sampling one never reads its
// neighbour. A glyph's write covers its border too, so a cleared page needs
// no wiping: whatever lies under a new glyph is overwritten.
constexpr uint16_t BORDER = 1;

} // namespace

std::size_t GlyphAtlas::KeyHash::operator()(const Key& key) const {
    std::size_t hash = key.face;
    hash = hash * 1000003u ^ key.glyph;
    hash = hash * 1000003u ^ static_cast<uint32_t>(key.size);
    return hash;
}

GlyphAtlas::GlyphAtlas(AtlasOptions options) : options_{options} {}

void GlyphAtlas::begin_frame() {
    ++frame_;
}

std::optional<AtlasRect> GlyphAtlas::place(Page& page, uint16_t width, uint16_t height) const {
    const int size = options_.page_size;
    // The lowest shelf it fits on, so taller shelves are left for taller
    // glyphs. One much taller than the glyph would waste its height, so a
    // new shelf is cut instead while the page has room for one.
    Shelf* best = nullptr;
    for (Shelf& shelf : page.shelves) {
        if (shelf.height >= height && size - shelf.x >= width && (best == nullptr || shelf.height < best->height)) {
            best = &shelf;
        }
    }
    const bool room_for_shelf = size - page.next_y >= height;
    if (best != nullptr && best->height > height + height / 2 && room_for_shelf) best = nullptr;
    if (best == nullptr) {
        if (!room_for_shelf) return std::nullopt;
        page.shelves.push_back({.y = page.next_y, .height = height, .x = 0});
        page.next_y = static_cast<uint16_t>(page.next_y + height);
        best = &page.shelves.back();
    }
    const AtlasRect rect{.x = best->x, .y = best->y, .width = width, .height = height};
    best->x = static_cast<uint16_t>(best->x + width);
    return rect;
}

std::optional<std::pair<uint16_t, AtlasRect>> GlyphAtlas::allocate(uint16_t width, uint16_t height) {
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (auto rect = place(pages_[i], width, height)) return std::pair{static_cast<uint16_t>(i), *rect};
    }
    if (pages_.size() < options_.max_pages) {
        const std::size_t size = options_.page_size;
        pages_.push_back(Page{.pixels = std::vector<uint8_t>(size * size, 0)});
        const auto index = static_cast<uint16_t>(pages_.size() - 1);
        if (auto rect = place(pages_.back(), width, height)) return std::pair{index, *rect};
        return std::nullopt;
    }
    // Every page is full: the one used longest ago is cleared, unless it is
    // in use this frame too.
    std::size_t oldest = 0;
    for (std::size_t i = 1; i < pages_.size(); ++i) {
        if (pages_[i].last_used < pages_[oldest].last_used) oldest = i;
    }
    Page& page = pages_[oldest];
    if (page.last_used >= frame_) return std::nullopt;
    for (const Key& key : page.keys) entries_.erase(key);
    page.keys.clear();
    page.shelves.clear();
    page.next_y = 0;
    if (auto rect = place(page, width, height)) return std::pair{static_cast<uint16_t>(oldest), *rect};
    return std::nullopt;
}

std::optional<AtlasGlyph> GlyphAtlas::glyph(const Face& face, uint32_t index, float size) {
    const Key key{.face = face.id(), .glyph = index, .size = static_cast<int32_t>(std::lround(size * 64.0f))};
    if (auto found = entries_.find(key); found != entries_.end()) {
        if (!found->second.glyph.rect.empty()) pages_[found->second.glyph.page].last_used = frame_;
        return found->second.glyph;
    }

    // Rasterized at exactly the size, 72 dots to the inch making a point a
    // pixel. Light hinting fits the outline to the pixel grid vertically
    // only, so the advances shaping gave still hold.
    FT_Face ft = face.impl().ft;
    if (FT_Set_Char_Size(ft, 0, key.size, 72, 72) != 0 ||
        FT_Load_Glyph(ft, index, FT_LOAD_TARGET_LIGHT | FT_LOAD_RENDER) != 0) {
        return std::nullopt;
    }
    const FT_GlyphSlot slot = ft->glyph;
    const FT_Bitmap& bitmap = slot->bitmap;
    AtlasGlyph placed{.bearing = {static_cast<float>(slot->bitmap_left), -static_cast<float>(slot->bitmap_top)}};
    // A glyph without pixels (a space), or in colour, which this atlas
    // doesn't hold, takes no room.
    if (bitmap.width == 0 || bitmap.rows == 0 || bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
        entries_.emplace(key, Entry{placed});
        return placed;
    }
    const unsigned int padded_width = bitmap.width + 2u * BORDER;
    const unsigned int padded_height = bitmap.rows + 2u * BORDER;
    if (padded_width > options_.page_size || padded_height > options_.page_size) return std::nullopt;
    const auto spot = allocate(static_cast<uint16_t>(padded_width), static_cast<uint16_t>(padded_height));
    if (!spot) return std::nullopt;

    const auto [page_index, padded] = *spot;
    Page& page = pages_[page_index];
    const std::size_t stride = options_.page_size;
    for (unsigned int row = 0; row < padded.height; ++row) {
        uint8_t* out = page.pixels.data() + (padded.y + row) * stride + padded.x;
        std::fill_n(out, padded.width, uint8_t{0});
        if (row < BORDER || row >= BORDER + bitmap.rows) continue;
        const unsigned int source_row = row - BORDER;
        const unsigned char* in =
            bitmap.pitch >= 0 ? bitmap.buffer + static_cast<std::ptrdiff_t>(source_row) * bitmap.pitch
                              : bitmap.buffer + static_cast<std::ptrdiff_t>(bitmap.rows - 1 - source_row) * -bitmap.pitch;
        std::copy_n(in, bitmap.width, out + BORDER);
    }

    placed.page = page_index;
    placed.rect = {.x = static_cast<uint16_t>(padded.x + BORDER),
                   .y = static_cast<uint16_t>(padded.y + BORDER),
                   .width = static_cast<uint16_t>(bitmap.width),
                   .height = static_cast<uint16_t>(bitmap.rows)};
    page.last_used = frame_;
    page.keys.push_back(key);
    writes_.push_back({.page = page_index, .rect = padded});
    entries_.emplace(key, Entry{placed});
    return placed;
}

std::vector<AtlasWrite> GlyphAtlas::take_writes() {
    return std::exchange(writes_, {});
}

} // namespace ftk::text
