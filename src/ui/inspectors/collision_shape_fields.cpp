#include "ui/inspectors/collision_shape_fields.hpp"

#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/row.hpp"

#include <imgui.h>

#include <format>

namespace fjell {

namespace {

constexpr ui::DragSpec POSITION{.speed = 0.005F, .unit = ui::Unit::Metres, .format = "%.3f"};
constexpr ui::DragSpec SIZE{.speed = 0.005F, .min = 0.001F, .max = 1000.0F, .unit = ui::Unit::Metres,
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

ui::Edit collision_shape_rows(CollisionPrimitive& shape, const char* position_help) {
    ui::Edit edit;
    if (shape.kind == CollisionPrimitiveKind::convex_hull || shape.kind == CollisionPrimitiveKind::triangle_mesh) {
        ui::hint("Built from the geometry; nothing to size");
        return edit;
    }
    ui::row("Position", position_help, [&] { edit |= ui::vec3("##position", shape.position, POSITION); });
    switch (shape.kind) {
        case CollisionPrimitiveKind::box:
            ui::row("Half extents", [&] { edit |= ui::vec3("##extents", shape.extents, SIZE); });
            break;
        case CollisionPrimitiveKind::sphere:
            ui::row("Radius", [&] { edit |= ui::drag("##radius", shape.extents.x, SIZE); });
            break;
        case CollisionPrimitiveKind::capsule:
            ui::row("Radius", [&] { edit |= ui::drag("##radius", shape.extents.x, SIZE); });
            ui::row("Height", [&] { edit |= ui::drag("##height", shape.extents.y, SIZE); });
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
    ui::menu_heading(fitted.c_str());
    for (const auto kind : KINDS) {
        ImGui::PushID(static_cast<int>(kind));
        if (ui::menu_item({.label = collision_kind_name(kind)})) pick = ShapePick{kind, true};
        ImGui::PopID();
    }
    ImGui::Separator();
    ui::menu_heading("Unit size");
    for (const auto kind : KINDS) {
        ImGui::PushID(100 + static_cast<int>(kind));
        if (ui::menu_item({.label = collision_kind_name(kind)})) pick = ShapePick{kind, false};
        ImGui::PopID();
    }
    ImGui::EndPopup();
    return pick;
}

} // namespace fjell
