#pragma once

#include "scripting/script_manager.hpp" // OwnedFieldDesc

#include <vector>

namespace fjell {

/// Draws inspector widgets for an instance's reflected fields, grouped by
/// category: numbers, vectors, colours, enums, strings, and asset and node
/// references resolved through the running script services. Text commits on
/// Enter or when the field loses focus. Returns true when a field changed.
/// Used for script components and animation notifies alike.
bool draw_reflected_fields(const std::vector<OwnedFieldDesc>& fields, void* base_ptr);

} // namespace fjell
