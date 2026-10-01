#include "ftk/ui/kit/grouped_picker.hpp"
#include "imgui_harness.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "ui/node_slot.hpp"
#include "ui/node_visuals.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using ftk::test::ImGuiHarness;
using namespace fjell;

namespace {

// A cave holding a goblin that carries the component the slot wants, and a
// crate that does not.
struct Cave {
    Scene scene;
    uint32_t cave{0};
    uint32_t goblin{0};
    uint32_t crate{0};

    Cave() {
        cave = scene.add_folder("Cave");
        goblin = scene.add_empty({}, "Goblin");
        crate = scene.add_empty({}, "Crate");
        scene.find(goblin)->parent_id = static_cast<int32_t>(cave);
        ScriptEntry stats;
        stats.type_name = "EnemyStats";
        scene.find(goblin)->scripts.push_back(std::move(stats));
    }

    [[nodiscard]] NodeSlotConfig config() const {
        return {.id = "##target",
                .scene = &scene,
                .refusal = [](const Node& node) -> std::string {
                    for (const auto& script : node.scripts) {
                        if (script.type_name == "EnemyStats") return {};
                    }
                    return node.name + " has no EnemyStats";
                },
                .empty_glyph = nullptr,
                .empty_text = "This node"};
    }
};

// The slot under a button that drags `dragged` the way the hierarchy does.
struct SlotUnderTest {
    ImGuiHarness h;
    Cave cave;
    uint32_t node{0};
    uint32_t dragged{0};
    int changes{0};
    ImVec2 field{};

    SlotUnderTest() {
        h.set_ui([this] {
            ImGui::Button("Goblin in the hierarchy");
            h.mark("source");
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(NODES_PAYLOAD, &dragged, sizeof(dragged));
                ImGui::EndDragDropSource();
            }
            field = ImGui::GetCursorScreenPos();
            ImGui::SetNextItemWidth(240.0f);
            if (node_slot(cave.config(), node)) ++changes;
            h.mark("clear");
        });
        h.step(2);
    }
    ~SlotUnderTest() { ftk::ui::grouped_picker::close(); }

    [[nodiscard]] ImVec2 into_field() const { return {field.x + 20.0f, field.y + ImGui::GetFrameHeight() * 0.5f}; }
    [[nodiscard]] ImVec2 source() const {
        const ImVec2 a = h.rect_min("source");
        const ImVec2 b = h.rect_max("source");
        return {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    }
};

} // namespace

TEST_CASE("A node slot's picker offers only the nodes that fit", "[ui][node_slot]") {
    SlotUnderTest t;
    t.h.click_at(t.into_field());
    t.h.step(2);
    t.h.press(ImGuiKey_DownArrow); // the crate is not listed: there is nothing past the goblin
    t.h.press(ImGuiKey_Enter);
    CHECK(t.node == t.cave.goblin);
    CHECK(t.changes == 1);
}

TEST_CASE("A node dragged from the hierarchy is taken only when it fits", "[ui][node_slot]") {
    SlotUnderTest t;
    t.dragged = t.cave.crate;
    t.h.drag(t.source(), t.into_field());
    CHECK(t.node == 0);
    CHECK(t.changes == 0);

    t.dragged = t.cave.goblin;
    t.h.drag(t.source(), t.into_field());
    CHECK(t.node == t.cave.goblin);
    CHECK(t.changes == 1);
}

TEST_CASE("Clearing a node slot empties it", "[ui][node_slot]") {
    SlotUnderTest t;
    t.node = t.cave.goblin;
    t.h.step();
    t.h.click("clear");
    CHECK(t.node == 0);
    CHECK(t.changes == 1);
}

TEST_CASE("A required node slot says so under itself while it is empty", "[ui][node_slot]") {
    ImGuiHarness h;
    Cave cave;
    uint32_t node = 0;
    bool required = true;
    float below = 0.0f;
    h.set_ui([&] {
        NodeSlotConfig config = cave.config();
        config.required = required;
        ImGui::SetNextItemWidth(240.0f);
        node_slot(config, node);
        below = ImGui::GetCursorScreenPos().y;
    });

    h.step(2);
    const float empty_required = below;
    node = cave.goblin;
    h.step(2);
    const float set_required = below;
    node = 0;
    required = false;
    h.step(2);
    const float empty_optional = below;

    CHECK(empty_required > set_required);
    CHECK(empty_optional == set_required);
    ftk::ui::grouped_picker::close();
}

TEST_CASE("A node's parent path runs from the root down", "[ui][node_slot]") {
    Cave cave;
    const uint32_t spawns = cave.scene.add_empty({}, "Spawns");
    cave.scene.find(spawns)->parent_id = static_cast<int32_t>(cave.cave);
    cave.scene.find(cave.goblin)->parent_id = static_cast<int32_t>(spawns);

    CHECK(node_parent_path(cave.scene, *cave.scene.find(cave.goblin)) == "Cave / Spawns");
    CHECK(node_parent_path(cave.scene, *cave.scene.find(cave.cave)).empty());
}
