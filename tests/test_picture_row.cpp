#include "kit_harness.hpp"

#include "ftk/ui/core/draw_list.hpp"
#include "ftk/ui/kit/picture_row.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// Two rows, the picked one tracked as a list's owner tracks it, with a menu
// on each row as a template of the user's has.
struct Rows {
    KitHarness h;
    int picked{0};
    int opened{-1};
    int double_clicked{-1};
    bool menu{false};

    Rows() {
        h.set_ui([this] {
            constexpr std::array<const char*, 2> NAMES{"Empty", "Harbour base"};
            for (int i = 0; i < 2; ++i) {
                const auto row = ui::picture_row({.id = NAMES[static_cast<std::size_t>(i)],
                                                  .icon = "?",
                                                  .name = NAMES[static_cast<std::size_t>(i)],
                                                  .detail = "A line about it"},
                                                 picked == i);
                h.mark(NAMES[static_cast<std::size_t>(i)]);
                if (row.clicked) picked = i;
                if (row.double_clicked) double_clicked = i;
                if (auto context = ui::ContextMenu("##row")) {
                    menu = true;
                    opened = i;
                }
            }
        });
        h.step(2);
    }
};

} // namespace

TEST_CASE("A picture row is picked by a click", "[ui][picture_row]") {
    Rows r;
    CHECK(r.picked == 0);
    r.h.click("Harbour base");
    CHECK(r.picked == 1);
    r.h.click("Empty");
    CHECK(r.picked == 0);
}

TEST_CASE("A picture row says when it is double-clicked", "[ui][picture_row]") {
    Rows r;
    r.h.double_click("Harbour base");
    CHECK(r.double_clicked == 1);
}

TEST_CASE("A picture row is the item its owner's menu opens on", "[ui][picture_row]") {
    Rows r;
    r.h.click("Harbour base", ui::MouseButton::Right);
    r.h.step();
    CHECK(r.menu);
    CHECK(r.opened == 1);
}

TEST_CASE("Picture rows stack, each as wide as the space", "[ui][picture_row]") {
    Rows r;
    const auto first = r.h.rect("Empty");
    const auto second = r.h.rect("Harbour base");
    CHECK(second.min.y >= first.max.y);
    CHECK(first.width() == second.width());
    CHECK(first.height() > 36.0f);
}

namespace {

// Three projects as the hub lists them: one with a picture, one without,
// and one whose folder is gone, searched for "fj".
struct LargeRows {
    KitHarness h;
    int clicked{-1};
    bool menu{false};
    std::string query{"fj"};

    LargeRows() {
        h.set_ui([this] {
            const std::array<ui::PictureRowSpec, 3> rows{{
                {.id = "fjara", .picture = ui::Texture{5}, .name = "fjara", .detail = "Opened 2 hours ago",
                 .size = ui::PictureRowSize::large, .path = "~/fjara", .query = query},
                {.id = "plain", .icon = "?", .name = "TestProject", .detail = "Opened 12 Oct",
                 .size = ui::PictureRowSize::large, .path = "~/Fjell/build/test_project", .query = query},
                {.id = "gone", .picture = ui::Texture{6}, .name = "storm-proto", .size = ui::PictureRowSize::large,
                 .path = "/run/media/kay/archive/storm-proto", .problem = "Folder not found", .unavailable = true,
                 .tooltip = "Moved, renamed, or on a drive that isn't connected", .query = query},
            }};
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (ui::picture_row(rows[i], false).clicked) clicked = static_cast<int>(i);
                h.mark(rows[i].id);
                if (auto context = ui::ContextMenu("##row")) menu = true;
            }
        });
        h.step(2);
    }

    // The primitives the last frame drew showing picture `texture`.
    [[nodiscard]] std::vector<ftk::ui::core::Primitive> showing(uint32_t texture) const {
        std::vector<ftk::ui::core::Primitive> found;
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                // A glyph's texture is its atlas page, not a picture.
                if (p.kind != ftk::ui::core::PrimitiveKind::Rect || p.texture == 0) continue;
                if (root.draw->textures[p.texture] == ui::Texture{texture}) found.push_back(p);
            }
        }
        return found;
    }

    // How many glyphs the last frame drew in `colour`.
    [[nodiscard]] int glyphs_in(ftk::ui::Color colour) const {
        const uint32_t packed = ftk::ui::core::pack_colour(colour);
        int count = 0;
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.kind == ftk::ui::core::PrimitiveKind::Glyph && p.colour == packed) ++count;
            }
        }
        return count;
    }
};

} // namespace

TEST_CASE("A large picture row holds a 128 by 72 picture with room around it", "[ui][picture_row]") {
    LargeRows r;
    // Sheet 55: the picture and six units above and below it.
    CHECK(r.h.rect("fjara").height() == 84.0f);
    CHECK(ui::picture_row_height(ui::PictureRowSize::large) == 84.0f);
    const auto pictures = r.showing(5);
    REQUIRE(pictures.size() == 1);
    CHECK(pictures[0].max.x - pictures[0].min.x == 128.0f);
    CHECK(pictures[0].max.y - pictures[0].min.y == 72.0f);
}

TEST_CASE("A row whose thing can't be used shows its picture in greys and says so", "[ui][picture_row]") {
    LargeRows r;
    const auto gone = r.showing(6);
    REQUIRE(gone.size() == 1);
    CHECK(gone[0].flags == ftk::ui::core::PRIMITIVE_GREY);
    const auto kept = r.showing(5);
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].flags == 0u);

    const auto* node = r.h.find("storm-proto", ftk::ui::core::Role::ListItem);
    REQUIRE(node != nullptr);
    CHECK(node->state.disabled);
    // Its problem in the error colour, behind the error icon.
    CHECK(r.glyphs_in(ftk::theme::error()) >= 2);
}

TEST_CASE("A row whose thing can't be used still takes clicks, its menu and its tooltip", "[ui][picture_row]") {
    LargeRows r;
    r.h.click("gone");
    CHECK(r.clicked == 2);
    r.h.click("gone", ui::MouseButton::Right);
    r.h.step();
    CHECK(r.menu);
}

TEST_CASE("A row's tooltip shows while the mouse rests on it", "[ui][picture_row]") {
    LargeRows r;
    r.h.hover("gone");
    r.h.step(60);
    CHECK(r.h.tooltip().has_value());
}

TEST_CASE("The search's match is amber in a large row's name and path", "[ui][picture_row]") {
    LargeRows r;
    // "fj" in fjara's name and folder, in TestProject's folder (Fjell), in
    // none of storm-proto's: two glyphs each.
    CHECK(r.glyphs_in(ftk::theme::accent()) == 6);
    r.query.clear();
    r.h.step();
    CHECK(r.glyphs_in(ftk::theme::accent()) == 0);
}
