#include "ui/inspectors/collision_shape_fields.hpp"

#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/row.hpp"

#include <imgui.h>

#include <format>

namespace fjell {

namespace {

constexpr ftk::ui::DragSpec POSITION{.speed = 0.005F, .unit = ftk::ui::Unit::Metres, .format = "%.3f"};
constexpr ftk::ui::DragSpec SIZE{.speed = 0.005F, .min = 0.001F, .max = 1000.0F, .unit = ftk::ui::Unit::Metres,
                            .format = "%.3f"};

} // namespace

const char* collision_kind_name(CollisionPrimitiveKind kind) {
    switch (kind) {
        case CollisionPrimitiveKind::box: return "Box";
        case CollisionPrimitiveKind::sphere: return "Sphere";
        case CollisionPrimitiveKind::capsule: return "Capsule";
        case CollisionPrimitiveKind::convex_hull: return "Convex hull";
        case CollisionPrimitiveKind::triangle_mesh: return "Triangle mesh";
    }
    return "Shape";
}

std::string collision_shape_summary(const CollisionPrimitive& shape) {
    const auto& e = shape.extents;
    switch (shape.kind) {
        case CollisionPrimitiveKind::box:
            return std::format("Box \xc2\xb7 {:.2f} \xc3\x97 {:.2f} \xc3\x97 {:.2f} m", e.x * 2.0F, e.y * 2.0F, e.z * 2.0F);
        case CollisionPrimitiveKind::sphere:
            return std::format("Sphere \xc2\xb7 radius {:.2f} m", e.x);
        case CollisionPrimitiveKind::capsule:
            return std::format("Capsule \xc2\xb7 radius {:.2f} m, height {:.2f} m", e.x, e.y);
        default:
            return collision_kind_name(shape.kind);
    }
}

ftk::ui::Edit collision_shape_rows(CollisionPrimitive& shape, const char* position_help) {
    ftk::ui::Edit edit;
    if (shape.kind == CollisionPrimitiveKind::convex_hull || shape.kind == CollisionPrimitiveKind::triangle_mesh) {
        ftk::ui::hint("Built from the geometry; nothing to size");
        return edit;
    }
    ftk::ui::row("Position", position_help, [&] { edit |= ftk::ui::vec3("##position", shape.position, POSITION); });
    switch (shape.kind) {
        case CollisionPrimitiveKind::box:
            ftk::ui::row("Half extents", [&] { edit |= ftk::ui::vec3("##extents", shape.extents, SIZE); });
            break;
        case CollisionPrimitiveKind::sphere:
            ftk::ui::row("Radius", [&] { edit |= ftk::ui::drag("##radius", shape.extents.x, SIZE); });
            break;
        case CollisionPrimitiveKind::capsule:
            ftk::ui::row("Radius", [&] { edit |= ftk::ui::drag("##radius", shape.extents.x, SIZE); });
            ftk::ui::row("Height", [&] { edit |= ftk::ui::drag("##height", shape.extents.y, SIZE); });
            break;
        default:
            break;
    }
    return edit;
}

std::optional<ShapePick> add_shape_menu(const char* id, const char* fit_to) {
    std::optional<ShapePick> pick;
    if (!ImGui::BeginPopup(id)) return pick;
    static constexpr CollisionPrimitiveKind KINDS[] = {
        CollisionPrimitiveKind::box, CollisionPrimitiveKind::sphere, CollisionPrimitiveKind::capsule};
    const std::string fitted = std::string("Fitted to ") + fit_to;
    ftk::ui::menu_heading(fitted.c_str());
    for (const auto kind : KINDS) {
        ImGui::PushID(static_cast<int>(kind));
        if (ftk::ui::menu_item({.label = collision_kind_name(kind)})) pick = ShapePick{kind, true};
        ImGui::PopID();
    }
    ImGui::Separator();
    ftk::ui::menu_heading("Unit size");
    for (const auto kind : KINDS) {
        ImGui::PushID(100 + static_cast<int>(kind));
        if (ftk::ui::menu_item({.label = collision_kind_name(kind)})) pick = ShapePick{kind, false};
        ImGui::PopID();
    }
    ImGui::EndPopup();
    return pick;
}

} // namespace fjell
