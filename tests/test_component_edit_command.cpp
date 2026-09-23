#include "core/command_history.hpp"
#include "core/commands.hpp"
#include "scene/scene.hpp"
#include "ui/edit_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace fjell;

namespace {

struct Health {
    int hp{100};
};
struct Armour {
    int value{5};
};
struct Tag {
    int id{0};
};

// A copy of a node's component of type T, as the Properties panel keeps one.
template <typename T>
ComponentStorage snapshot(Node& node) {
    ComponentStorage copy;
    node.components.clone_one(component_type_id<T>(), node.components, copy);
    return copy;
}

} // namespace

TEST_CASE("An edited component is put back as it was, in its place", "[components][undo]") {
    Scene scene;
    const uint32_t id = scene.add_empty({}, "crate");
    Node& node = *scene.find(id);
    node.components.add<Armour>();
    node.components.add<Health>();
    node.components.add<Tag>();

    ComponentStorage before = snapshot<Health>(node);
    node.components.get<Health>()->hp = 40;
    ComponentStorage after = snapshot<Health>(node);

    ComponentEditCommand edit{scene, id, component_type_id<Health>(), std::move(before),
                              std::move(after), "Edit Health"};

    // The first execute finds the edit already on the node and leaves the
    // component alone.
    const Health* live = node.components.get<Health>();
    edit.execute();
    CHECK(node.components.get<Health>() == live);
    CHECK(live->hp == 40);

    edit.undo();
    CHECK(node.components.get<Health>()->hp == 100);
    CHECK(node.components.index_of(component_type_id<Health>()) == 1);

    edit.execute();
    CHECK(node.components.get<Health>()->hp == 40);
    CHECK(node.components.index_of(component_type_id<Health>()) == 1);
    CHECK(node.components.count() == 3);
}

TEST_CASE("Marking the scene dirty moves its revision", "[components][undo]") {
    Scene scene;
    const uint64_t start = scene.revision();
    scene.mark_dirty();
    scene.mark_dirty();
    CHECK(scene.revision() == start + 2);
}

namespace {

// A node with one Health component, its tracker and history, and the frames
// the Properties panel would report for it.
struct Fixture {
    Scene scene;
    CommandHistory history;
    EditTracker<ComponentStorage> tracker;
    uint32_t id{scene.add_empty({}, "crate")};
    Node& node{*scene.find(id)};
    Health& health{node.components.add<Health>()};

    Health& live() { return *node.components.get<Health>(); }
    void frame(EditFrame f) {
        const ComponentTypeId type = component_type_id<Health>();
        tracker.track(
            scene, &history, id, type, f,
            [&] { return snapshot<Health>(node); },
            [&](ComponentStorage before, ComponentStorage after) {
                return std::make_unique<ComponentEditCommand>(scene, id, type, std::move(before),
                                                              std::move(after), "Edit Health");
            });
    }
    // A drag from the current value to `to` over three frames, let go on
    // the fourth.
    void drag_to(int to) {
        frame({.hovered = true});
        for (int i = 1; i <= 3; ++i) {
            live().hp += (to - live().hp) / (4 - i);
            frame({.changed = true, .active = true, .hovered = true});
        }
        live().hp = to;
        frame({.changed = true, .committed = true, .hovered = true});
    }
};

} // namespace

TEST_CASE("A drag across frames is one undo step", "[components][undo]") {
    Fixture f;
    f.frame({});
    f.drag_to(40);
    REQUIRE(f.history.commands().size() == 1);
    CHECK(f.history.commands()[0]->description() == "Edit Health");
    f.history.undo();
    CHECK(f.live().hp == 100);
    f.history.redo();
    CHECK(f.live().hp == 40);
}

TEST_CASE("A change that isn't committed yet is no undo step", "[components][undo]") {
    Fixture f;
    f.frame({});
    f.live().hp = 60;
    f.frame({.changed = true, .active = true});
    CHECK(f.history.commands().empty());
    CHECK(f.scene.is_dirty());
}

TEST_CASE("After a change from elsewhere, undo returns to that change once the mouse comes back",
          "[components][undo]") {
    Fixture f;
    f.frame({});
    // Something outside the block sets the value while the mouse is away.
    f.live().hp = 70;
    f.scene.mark_dirty();
    f.frame({});
    f.frame({.hovered = true});
    f.drag_to(40);
    f.history.undo();
    CHECK(f.live().hp == 70);
}

TEST_CASE("A change from elsewhere during an edit doesn't move where undo returns to",
          "[components][undo]") {
    Fixture f;
    f.frame({});
    f.live().hp = 90;
    f.frame({.changed = true, .active = true, .hovered = true});
    f.scene.mark_dirty();  // the gizmo, a script
    f.frame({.active = true, .hovered = true});
    f.live().hp = 50;
    f.frame({.changed = true, .committed = true, .hovered = true});
    f.history.undo();
    CHECK(f.live().hp == 100);
}

TEST_CASE("Entering a field from the keyboard renews the copy undo returns to",
          "[components][undo]") {
    Fixture f;
    f.frame({});
    f.live().hp = 70;
    f.scene.mark_dirty();
    // Tabbed into, with the mouse elsewhere: active, nothing changed yet.
    f.frame({.active = true});
    f.live().hp = 10;
    f.frame({.changed = true, .active = true});
    f.frame({.committed = true});
    f.history.undo();
    CHECK(f.live().hp == 70);
}
