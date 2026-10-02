#pragma once

// What the UI keeps for a widget from one frame to the next, under an id in
// the window being drawn: a fold, the row picked, a width last measured.
// Kept for the session, not saved.
//
//     const ui::Id picked = ui::id("##picked");
//     int row = ui::stored_int(picked, -1);
//     if (clicked) ui::store_int(picked, row = i);

#include "ftk/ui/types.hpp"

namespace ftk::ui {

/// What is kept under `id`, or `fallback` when nothing is yet.
[[nodiscard]] int stored_int(Id id, int fallback = 0);
[[nodiscard]] bool stored_bool(Id id, bool fallback = false);
[[nodiscard]] float stored_float(Id id, float fallback = 0.0f);

/// Keeps `value` under `id`.
void store_int(Id id, int value);
void store_bool(Id id, bool value);
void store_float(Id id, float value);

} // namespace ftk::ui
