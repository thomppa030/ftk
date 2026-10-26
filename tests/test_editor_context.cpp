#include "kit_harness.hpp"

#include "ftk/app/command.hpp"
#include "ftk/app/editor_context.hpp"
#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
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

// An editor with a tree and an inspector, laid out unlike Laid.
class Sheet : public Notes {
public:
    [[nodiscard]] DockPreset dock_preset() const override { return {.tree = {"Rows"}, .inspector = {"Cells"}}; }
};

// The row of context tabs, as the engine keeps it: each context given an
// id as it opens, its dockspace given every frame and shown while its tab
// is in front, laid out from its preset the first frame it is seen; the
// front one's panels drawn.
class Row {
public:
    std::vector<std::unique_ptr<ftk::EditorContext>> contexts;
    std::size_t front = 0;
    int next_id = 0;

    void open(std::unique_ptr<ftk::EditorContext> context) {
        context->set_context_id(next_id++);
        contexts.push_back(std::move(context));
        front = contexts.size() - 1;
    }

    void close(std::size_t at) {
        contexts.erase(contexts.begin() + static_cast<std::ptrdiff_t>(at));
        if (at < front) --front;
    }

    void draw() {
        if (contexts.empty()) return;
        for (std::size_t i = 0; i < contexts.size(); ++i) contexts[i]->draw_dockspace(i == front);
        for (const auto& context : contexts) {
            if (context->dockspace_built()) continue;
            context->mark_dockspace_built();
            ui::DockBuild build(context->dockspace_id(), {800.0f, 600.0f});
            context->setup_dockspace(context->dockspace_id());
        }
        for (const std::string& name : contexts[front]->docked_window_names()) {
            if (auto panel = ui::Panel("", name.c_str())) ui::text(name);
        }
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
        laid.draw_dockspace(true);
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

TEST_CASE("An editor's panels stay docked when a tab before it closes", "[ui][kit][dock]") {
    KitHarness h;
    Row row;
    h.set_ui([&] { row.draw(); });
    row.open(std::make_unique<Notes>());
    h.step(2);
    row.open(std::make_unique<Laid>());
    h.step(2);
    row.open(std::make_unique<Sheet>());
    h.step(2);
    ftk::EditorContext& sheet = *row.contexts[2];
    const std::optional<ui::DockNode> before = ui::dock_tree(sheet.dockspace_id(), sheet.docked_window_names());
    REQUIRE(before.has_value());

    row.close(1);
    h.step(2);
    for (const std::string& name : sheet.docked_window_names()) {
        INFO(name);
        CHECK(ui::window_dock(name) != 0);
    }
    CHECK(ui::dock_tree(sheet.dockspace_id(), sheet.docked_window_names()) == before);
}
