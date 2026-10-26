#include "kit_harness.hpp"

#include "ftk/app/command.hpp"
#include "ftk/app/editor_context.hpp"
#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using Catch::Approx;
using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// A step that changes nothing, described as it is told.
class Step : public ftk::Command {
public:
    explicit Step(std::string description) : description_{std::move(description)} {}
    void execute() override {}
    void undo() override {}
    [[nodiscard]] std::string description() const override { return description_; }

private:
    std::string description_;
};

// An editor of one line of text, opened when told.
class Notes : public ftk::EditorContext {
public:
    std::string text = "a";

    [[nodiscard]] const char* name() const override { return "Notes"; }
    [[nodiscard]] const char* context_type() const override { return "notes"; }
    void init() override {}
    void draw(float /*dt*/) override {}
    bool handle_shortcuts() override { return false; }
    [[nodiscard]] std::string document() const override { return text; }
    [[nodiscard]] ftk::Result<> restore(const std::string& with) override {
        text = with;
        return {};
    }
    void open() { document_opened(); }
    void edit(const std::string& description, std::string to) {
        text = std::move(to);
        command_history().record(std::make_unique<Step>(description));
    }
};

// An editor with a panel over its tree, as the widget editor's Palette.
class Laid : public Notes {
public:
    [[nodiscard]] DockPreset dock_preset() const override {
        return {.above_tree = {"Palette"}, .tree = {"Elements"}, .preview = {"Preview"}};
    }
};

} // namespace

TEST_CASE("An editor's changes count from where it was opened or last saved", "[app][history]") {
    KitHarness h;
    Notes notes;
    notes.edit("Add Oak 4", "b");
    notes.open();
    CHECK(notes.changes().empty());

    notes.edit("Delete Rock 3", "c");
    CHECK(notes.changes() == std::vector<std::string>{"Delete Rock 3"});
    REQUIRE(notes.save());
    CHECK(notes.changes().empty());
}

TEST_CASE("A panel over an editor's tree shares its column, above it", "[ui][kit][dock]") {
    KitHarness h;
    Laid laid;
    const std::vector<std::string> names = laid.docked_window_names();
    bool built = false;
    h.set_ui([&] {
        laid.set_dockspace_id(ui::id("##dock"));
        ui::dockspace(laid.dockspace_id());
        if (!built) {
            ui::DockBuild build(laid.dockspace_id(), {800.0f, 600.0f});
            laid.setup_dockspace(laid.dockspace_id());
            built = true;
        }
        for (const std::string& name : names) {
            if (auto panel = ui::Panel("", name.c_str())) ui::text(name);
        }
    });
    h.step(4);
    const auto tree = ui::dock_tree(laid.dockspace_id(), names);
    REQUIRE(tree.has_value());
    REQUIRE(tree->split == ui::DockAxis::LeftRight);
    const ui::DockNode& column = tree->children[0];
    REQUIRE(column.split == ui::DockAxis::TopBottom);
    CHECK(column.ratio == Approx(0.4f).margin(0.01f));
    CHECK(column.children[0].windows == std::vector<std::string>{laid.ctx_title("Palette")});
    CHECK(column.children[1].windows == std::vector<std::string>{laid.ctx_title("Elements")});
}
