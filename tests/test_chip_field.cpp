#include "kit_harness.hpp"

#include "ftk/ui/kit/chip_field.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// A chip field over `names`, taking a chip off as its × is clicked and
// counting the +'s presses.
struct Field {
    KitHarness h;
    std::vector<std::string> names;
    std::vector<std::string> problems;
    int adds = 0;

    explicit Field(std::vector<std::string> with, float width = 400.0f) : h({width, 300.0f}), names(std::move(with)) {
        problems.resize(names.size());
        h.set_ui([this] {
            std::vector<ui::Chip> chips;
            for (std::size_t i = 0; i < names.size(); ++i) chips.push_back({.text = names[i], .problem = problems[i]});
            const ui::ChipEdit edit = ui::chip_field("##tags", chips, "Add a tag");
            if (edit.removed) names.erase(names.begin() + static_cast<std::ptrdiff_t>(*edit.removed));
            if (edit.add) ++adds;
        });
        h.step(2);
    }
};

} // namespace

TEST_CASE("A chip field shows a chip a name and takes one off by its ×", "[ui][kit]") {
    Field f({"tree.oak", "shade"});
    REQUIRE(f.h.find("tree.oak"));
    REQUIRE(f.h.find("shade"));
    f.h.click("Take off tree.oak");
    f.h.step();
    CHECK(f.names == std::vector<std::string>{"shade"});
    f.h.click("Add a tag");
    f.h.step();
    CHECK(f.adds == 1);
}

TEST_CASE("A chip field wraps its chips onto more lines when they don't fit", "[ui][kit]") {
    Field many({"tree.oak.grey", "enemy.ranged", "interactable.door", "state.stunned", "creature.deer"}, 260.0f);
    const ui::core::Rect first = many.h.rect("tree.oak.grey");
    const ui::core::Rect last = many.h.rect("creature.deer");
    CHECK(last.min.y > first.max.y);
    // No chip, nor the +, past the window's edge.
    for (const char* name : {"tree.oak.grey", "enemy.ranged", "interactable.door", "state.stunned", "creature.deer",
                             "Add a tag"}) {
        CHECK(many.h.rect(name).max.x <= 260.0f);
    }
}

TEST_CASE("A chip naming nothing that is there says why", "[ui][kit]") {
    Field f({"tree.quercus"});
    f.problems[0] = "tree.quercus is not one of the project's tags";
    f.h.step();
    f.h.hover("tree.quercus");
    REQUIRE(f.h.tooltip());
}
