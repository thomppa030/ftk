#pragma once

#include "core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

/// A colour grading table as the GPU samples it: a strip of N slices, each N
/// by N, side by side, so the image is N·N wide and N tall. Within a slice red
/// runs left to right and green top to bottom; blue steps one slice at a time.
/// Unreal's layout, which is what a grade made in Photoshop on a screenshot
/// with the neutral strip pasted beside it comes back as.
///
/// Values are what the table maps a display colour to, in sRGB, 16 bits per
/// channel, RGBA with alpha at full.
struct LutStrip {
    uint32_t size{0};                ///< N, the slices and each slice's side
    std::vector<uint16_t> rgba;      ///< N·N × N texels, 4 channels, row by row

    [[nodiscard]] uint32_t width() const { return size * size; }
    [[nodiscard]] uint32_t height() const { return size; }
};

/// The table that changes nothing, at `size` (2 to 64).
[[nodiscard]] LutStrip neutral_lut(uint32_t size);

/// A strip from an image file's bytes: 8 or 16 bits per channel, N·N wide and
/// N tall for any N from 2 to 64 (16 and 32 are the usual).
[[nodiscard]] Result<LutStrip> decode_lut_strip(std::span<const uint8_t> image_bytes);

/// A strip from a Resolve / Adobe `.cube` 3D table: `LUT_3D_SIZE`, optional
/// `DOMAIN_MIN` and `DOMAIN_MAX`, then size³ rows with red changing fastest.
/// Resampled trilinearly onto a 32 strip.
[[nodiscard]] Result<LutStrip> decode_cube(std::string_view text);

/// A `.fjlut` recipe: `{"name": "Dusk", "source": "grading/dusk.png"}`. The
/// source is a strip image or a `.cube`, relative to the recipe's folder.
struct LutRecipe {
    std::string name;
    std::string source;
};
[[nodiscard]] Result<LutRecipe> read_lut_recipe(const std::filesystem::path& recipe_path);

/// Where a recipe's source file is.
[[nodiscard]] std::filesystem::path lut_source_path(const std::filesystem::path& recipe_path,
                                                   const LutRecipe& recipe);

/// The strip a `.fjlut` recipe names, read and decoded.
[[nodiscard]] Result<LutStrip> load_lut(const std::filesystem::path& recipe_path);

/// A strip as a 16-bit RGB PNG, for writing the neutral strips a grade
/// starts from.
[[nodiscard]] std::vector<uint8_t> encode_lut_png(const LutStrip& strip);

} // namespace fjell
