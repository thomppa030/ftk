#pragma once

// What a Face holds, for ftk-text's own files: the font file's bytes, and
// FreeType's and HarfBuzz's handles on them. Only ftk-text sees FreeType and
// HarfBuzz; nothing that links it can include this.

#include "ftk/text/face.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ftk::text {

struct Face::Impl {
    /// The file's bytes, which FreeType and HarfBuzz both read in place.
    std::vector<std::byte> data;
    uint32_t id{0};
    std::string family;
    float units_per_em{0.0f};

    /// For rasterizing glyphs. Sized for each glyph as it is drawn.
    FT_Face ft{nullptr};
    /// For shaping, scaled to the em in font units: shaped positions are
    /// scaled to pixels afterwards.
    hb_font_t* hb{nullptr};
};

} // namespace ftk::text
