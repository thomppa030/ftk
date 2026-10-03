#pragma once

// A colour's hex code, as the colour fields show it and take it typed:
// "#D4A054", or eight digits with alpha. The same for either kit.

#include <string>

namespace ftk::ui::detail {

/// The code of sRGB `rgba`: six digits, or eight with `alpha`.
[[nodiscard]] std::string to_hex(const float* rgba, bool alpha);

/// Reads "#RRGGBB" or "RRGGBB", plus "AA" when the colour has alpha, into
/// `rgba`. Leaves it alone and returns false for anything else.
bool from_hex(const std::string& text, float* rgba, bool alpha);

} // namespace ftk::ui::detail
