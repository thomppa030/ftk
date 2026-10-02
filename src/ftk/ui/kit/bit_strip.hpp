#pragma once

#include <cstddef>
#include <optional>
#include <span>

// A row of numbered squares, one per bit of a small mask, such as a node's
// marks (sheet 37, M7). A bit set here is filled in the toggle colour; one
// that comes from elsewhere is ringed and does nothing here; one that can be
// set is a plain square; one that cannot be (nothing is named for it) is dim
// and inert. Each square's tooltip names its bit. Under the strip, a
// bit_line() for each bit the thing carries says what it is.
//
//     if (const auto clicked = ui::bit_strip("##marks", bits)) toggle(*clicked);
//     for (const uint32_t bit : carried) ui::bit_line(bit, names[bit], inherited_from(bit));

namespace ftk::ui {

enum class BitState : unsigned char {
    /// Nothing is named for it: dim, and clicking does nothing.
    unavailable,
    off,
    on,
    /// Set by something else, a parent: ringed, and clicking does nothing.
    inherited,
};

/// One square of a bit_strip().
struct Bit {
    BitState state{BitState::off};
    /// Shown while the square is under the mouse: what the bit is.
    const char* tooltip{nullptr};
};

/// The strip, its squares numbered from 1. Returns the index of the square
/// clicked, to turn on or off; none while nothing is.
std::optional<std::size_t> bit_strip(const char* id, std::span<const Bit> bits);

/// A line under a bit_strip() for one bit the thing carries: the bit's number
/// (`index` + 1), its `label`, and, for one that comes from elsewhere, the
/// inherited icon and `from`.
void bit_line(std::size_t index, const char* label, const char* from = nullptr);

} // namespace ftk::ui
