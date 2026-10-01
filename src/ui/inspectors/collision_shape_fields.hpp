#pragma once

#include "ftk/ui/kit/edit.hpp"
#include "physics/collision_shape.hpp"

#include <optional>
#include <string>

// A collision shape's fields, shared by every editor that places them: a
// mesh's collision shapes and a skeleton's bone colliders.

namespace fjell {

/// The kind's name, "Box" or "Convex hull".
[[nodiscard]] const char* collision_kind_name(CollisionPrimitiveKind kind);

/// One line for a shape's card while it isn't selected: its kind and size
/// ("Box · 0.50 × 0.50 × 0.50 m").
[[nodiscard]] std::string collision_shape_summary(const CollisionPrimitive& shape);

/// A shape's fields as property rows: where it sits, with `position_help`
/// saying relative to what, then its size by kind (half extents, radius,
/// radius and height). A shape built from geometry says so instead.
ui::Edit collision_shape_rows(CollisionPrimitive& shape, const char* position_help);

/// A shape picked from the Add shape menu, sized to fit or at unit size.
struct ShapePick {
    CollisionPrimitiveKind kind{CollisionPrimitiveKind::box};
    bool fitted{false};
};

/// The Add shape menu, open after ImGui::OpenPopup(id): box, sphere and
/// capsule, each fitted to `fit_to` ("the mesh", "the bone") or at unit
/// size. Returns the pick on the frame it is made.
[[nodiscard]] std::optional<ShapePick> add_shape_menu(const char* id, const char* fit_to);

} // namespace fjell
