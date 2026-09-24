#include "imgui_harness.hpp"
#include "ui/inspectors/collision_shape_fields.hpp"
#include "ui/kit/row.hpp"

#include <catch2/catch_test_macros.hpp>

using fjell::test::ImGuiHarness;

TEST_CASE("A shape's card line names its kind and size", "[ui][collision]") {
    fjell::CollisionPrimitive box;
    box.kind = fjell::CollisionPrimitiveKind::box;
    box.extents = {0.5F, 1.0F, 0.25F};
    // Half extents are shown as the box's full size.
    CHECK(fjell::collision_shape_summary(box) == "Box \xc2\xb7 1.00 \xc3\x97 2.00 \xc3\x97 0.50 m");

    fjell::CollisionPrimitive capsule;
    capsule.kind = fjell::CollisionPrimitiveKind::capsule;
    capsule.extents = {0.2F, 1.8F, 0.0F};
    CHECK(fjell::collision_shape_summary(capsule) == "Capsule \xc2\xb7 radius 0.20 m, height 1.80 m");
}

TEST_CASE("A shape shows the size fields its kind has", "[ui][collision]") {
    ImGuiHarness h;
    fjell::CollisionPrimitive shape;
    int rows = 0;
    h.set_ui([&] {
        if (auto t = fjell::ui::PropertyTable("##shape")) {
            (void)fjell::collision_shape_rows(shape, "Relative to the mesh");
            rows = ImGui::TableGetRowIndex() + 1;
        }
    });
    shape.kind = fjell::CollisionPrimitiveKind::box;
    h.step(2);
    CHECK(rows == 2);  // position, half extents
    shape.kind = fjell::CollisionPrimitiveKind::capsule;
    h.step(2);
    CHECK(rows == 3);  // position, radius, height
    shape.kind = fjell::CollisionPrimitiveKind::convex_hull;
    h.step(2);
    CHECK(rows == 1);  // the hint that there is nothing to size
}
