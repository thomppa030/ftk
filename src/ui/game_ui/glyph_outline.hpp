#pragma once

// Merges a glyph's overlapping contours into the outline of what they fill.
// Many fonts draw a glyph as contours laid over each other, a bowl over a
// stem, and a distance field built from those keeps the edges that run
// inside the fill: text drawn from it shows them as seams. Part of the font
// baker, which is the only caller.

#include <msdfgen.h>

namespace fjell {

/// Rewrites `shape` as the boundary of the area it fills under the nonzero
/// rule. Edges are split where they cross, pieces with fill on both sides or
/// on neither are dropped, and the rest are chained into contours with the
/// fill on their right, the way TrueType winds an outline. Curves stay
/// curves. True when `shape` now fills exactly what it filled before, with
/// no edge inside the fill, including when it had nothing to merge and is
/// left as it was. False, with `shape` untouched, when the merged outline
/// could not be built or would fill differently: edges that run together
/// along a curve are the known case.
[[nodiscard]] bool merge_overlapping_contours(msdfgen::Shape& shape);

} // namespace fjell
