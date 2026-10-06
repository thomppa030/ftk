#pragma once

// What something costs against a whole a saving cut it down from (sheet 34,
// Q3), as a texture's memory against what it would take uncompressed: the
// cost in large type, then a track that is the whole, filled to the cost,
// the rest hatched with the saving written in it, so the saving is seen at
// once rather than worked out from figures.
//
//     const ui::MeterLevel levels[] = {{"High", high}, {"Medium", medium}, {"Low", low}};
//     ui::memory_meter("##memory", {.whole = uncompressed, .whole_name = "uncompressed",
//                                   .levels = levels, .in_use = 0,
//                                   .in_use_note = "the project's level", .in_use_tag = "project"});

#include <cstddef>
#include <cstdint>
#include <span>

namespace ftk::ui {

/// A level the cost can be at, such as a texture quality, and what it costs
/// there in bytes.
struct MeterLevel {
    const char* name{""};
    uint64_t bytes{0};
};

struct MemoryMeterSpec {
    /// The whole the track stands for, in bytes, and what it is
    /// ("uncompressed").
    uint64_t whole{0};
    const char* whole_name{""};
    /// The levels, the costliest first.
    std::span<const MeterLevel> levels{};
    /// The level in use, why ("the project's level") and its tag
    /// ("project").
    std::size_t in_use{0};
    const char* in_use_note{""};
    const char* in_use_tag{""};
};

/// Draws the meter across the space left: the cost at the level in use in
/// large type and the level it is at; a track that is the whole, filled to
/// the cost in the secondary text colour, the rest hatched with the saving
/// in it ("16.0 MiB saved · 75 %"), the cheaper levels as marks in the fill;
/// the scale's ends under it in the small mono face; then each level and
/// its cost on one line, the one in use tagged. Hovering a level shows the
/// meter at it.
/// @return the level shown: the one hovered, else the one in use
std::size_t memory_meter(const char* id, const MemoryMeterSpec& spec);

} // namespace ftk::ui
