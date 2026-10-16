#include "kit_harness.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/folder_field.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// A folder field over `folder` with a button under it to click away to,
// counting what the field reports. `live` and `greyed` set the field up as
// the New project dialog's and a locked form's.
struct Folder {
    KitHarness h;
    std::string folder{"assets/models"};
    bool live{false};
    bool greyed{false};
    float width{300.0f};
    int browses{0};
    int commits{0};
    int changed_frames{0};

    explicit Folder(bool live_field = false) : live(live_field) {
        h.set_ui([this] {
            const ui::Disabled off(greyed);
            ui::next_width(width);
            const ui::FolderField field = ui::folder_field("Output", folder, {.hint = "Beside each source", .live = live});
            h.mark("browse");
            browses += field.browse ? 1 : 0;
            commits += field.edit.committed ? 1 : 0;
            changed_frames += field.edit.changed ? 1 : 0;
            ui::button("Elsewhere");
            h.mark("elsewhere");
        });
        h.step(2);
    }

    [[nodiscard]] ui::core::Rect text() const {
        const ui::core::SemanticNode* node = h.find("Output", ui::core::Role::TextField);
        REQUIRE(node != nullptr);
        return node->rect;
    }

    // Clears the field's text: select all, then delete.
    void clear() {
        h.press(ui::Key::A, {.ctrl = true});
        h.step();
        h.press(ui::Key::Delete);
    }
};

} // namespace

TEST_CASE("A folder field's button says it was pressed and leaves the folder as it was", "[ui][folder_field]") {
    Folder f;
    f.h.click("browse");
    CHECK(f.browses == 1);
    CHECK(f.commits == 0);
    CHECK(f.folder == "assets/models");
    f.h.step(3);
    CHECK(f.browses == 1);
}

TEST_CASE("A folder field's button names what it does", "[ui][folder_field]") {
    Folder f;
    CHECK_FALSE(f.h.tooltip().has_value());
    f.h.hover("browse");
    CHECK(f.h.tooltip().has_value());
}

TEST_CASE("A folder field commits a folder typed in once, on Enter, as a text field does", "[ui][folder_field]") {
    Folder f;
    f.h.click_at(f.text().centre());
    f.clear();
    f.h.type("assets/models/Fjara");
    CHECK(f.changed_frames > 0);
    CHECK(f.commits == 0);
    CHECK(f.folder == "assets/models");

    f.h.press(ui::Key::Enter);
    CHECK(f.commits == 1);
    CHECK(f.folder == "assets/models/Fjara");
    CHECK(f.browses == 0);
}

TEST_CASE("A folder field commits when it is left, and Escape puts the folder back", "[ui][folder_field]") {
    Folder f;
    f.h.click_at(f.text().centre());
    f.clear();
    f.h.type("props");
    f.h.press(ui::Key::Escape);
    CHECK(f.commits == 0);
    CHECK(f.folder == "assets/models");

    f.h.click_at(f.text().centre());
    f.clear();
    f.h.type("props");
    f.h.click("elsewhere");
    CHECK(f.commits == 1);
    CHECK(f.folder == "props");
}

TEST_CASE("Typing in a folder field, then pressing its button, keeps what was typed", "[ui][folder_field]") {
    Folder f;
    f.h.click_at(f.text().centre());
    f.clear();
    f.h.type("props");
    f.h.click("browse");
    CHECK(f.commits == 1);
    CHECK(f.folder == "props");
    CHECK(f.browses == 1);
}

TEST_CASE("A folder field left empty commits nothing", "[ui][folder_field]") {
    Folder f;
    f.folder.clear();
    f.h.step();
    f.h.click_at(f.text().centre());
    f.h.click("elsewhere");
    CHECK(f.commits == 0);
    CHECK(f.folder.empty());
}

TEST_CASE("A live folder field writes the folder as it is typed, and Enter commits", "[ui][folder_field]") {
    Folder f(true);
    f.h.click_at(f.text().centre());
    f.clear();
    f.h.type("~/Projects");
    CHECK(f.folder == "~/Projects");
    CHECK(f.changed_frames > 0);
    CHECK(f.commits == 0);
    f.h.press(ui::Key::Enter);
    CHECK(f.commits == 1);
    CHECK(f.folder == "~/Projects");
}

TEST_CASE("A folder field and its button together take the width asked for", "[ui][folder_field]") {
    Folder f;
    const ui::core::Rect text = f.text();
    const ui::core::Rect button = f.h.rect("browse");
    CHECK(button.min.y == text.min.y);
    CHECK(button.height() == text.height());
    CHECK(button.width() == button.height());
    CHECK(button.min.x > text.max.x);
    CHECK(button.max.x - text.min.x == f.width);

    f.width = 180.0f;
    f.h.step();
    CHECK(f.h.rect("browse").max.x - f.text().min.x == 180.0f);
}

TEST_CASE("A greyed folder field's button can't be pressed", "[ui][folder_field]") {
    Folder f;
    f.greyed = true;
    f.h.step();
    f.h.click("browse");
    CHECK(f.browses == 0);
}

TEST_CASE("Two folder fields each answer their own button", "[ui][folder_field]") {
    KitHarness h;
    std::string first{"a"};
    std::string second{"b"};
    int first_browses = 0;
    int second_browses = 0;
    h.set_ui([&] {
        first_browses += ui::folder_field("##first", first).browse ? 1 : 0;
        h.mark("first");
        second_browses += ui::folder_field("##second", second).browse ? 1 : 0;
        h.mark("second");
    });
    h.step(2);
    h.click("second");
    CHECK(first_browses == 0);
    CHECK(second_browses == 1);
    h.click("first");
    CHECK(first_browses == 1);
    CHECK(second_browses == 1);
}
