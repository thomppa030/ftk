#include "ftk/test/imgui_harness.hpp"
#include "ftk/ui/kit/slot.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using ftk::test::ImGuiHarness;
using namespace ftk;

namespace {

constexpr const char* PAYLOAD = "TEST_ITEM";

// A slot taking named items, beside a button that drags `dragged` the way a
// list would. It refuses anything named "crate".
struct SlotUnderTest {
    ImGuiHarness h;
    std::string held;
    std::string dragged;
    int clicks{0};
    int clears{0};
    ImVec2 field{};

    SlotUnderTest() {
        h.set_ui([this] {
            ImGui::Button("Item in a list");
            h.mark("source");
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(PAYLOAD, dragged.c_str(), dragged.size() + 1);
                ImGui::EndDragDropSource();
            }
            field = ImGui::GetCursorScreenPos();
            ImGui::SetNextItemWidth(240.0f);
            ImGui::PushID("##item");
            ui::Slot slot(PAYLOAD, [](const ImGuiPayload& payload) -> std::string {
                const std::string name = static_cast<const char*>(payload.Data);
                return name == "crate" ? "Crates do not fit." : std::string();
            });
            h.mark("field");
            if (slot.clicked()) ++clicks;
            if (const ImGuiPayload* dropped = slot.dropped()) held = static_cast<const char*>(dropped->Data);
            std::string text = held.empty() ? "None" : held;
            if (slot.finish({.text = text}, held.empty(), {})) {
                held.clear();
                ++clears;
            }
            h.mark("clear");
            ImGui::PopID();
        });
        h.step(2);
    }

    [[nodiscard]] ImVec2 into_field() const { return {field.x + 20.0f, field.y + ImGui::GetFrameHeight() * 0.5f}; }
    [[nodiscard]] ImVec2 source() const {
        const ImVec2 a = h.rect_min("source");
        const ImVec2 b = h.rect_max("source");
        return {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    }
};

} // namespace

TEST_CASE("A slot hands over what is dropped on it once it fits", "[ui][slot]") {
    SlotUnderTest t;
    t.dragged = "crate";
    t.h.drag(t.source(), t.into_field());
    CHECK(t.held.empty());

    // The payload is read after the drop target has closed, which is when
    // ImGui clears its own: the slot keeps a copy.
    t.dragged = "goblin";
    t.h.drag(t.source(), t.into_field());
    CHECK(t.held == "goblin");
}

TEST_CASE("Clicking a slot's field asks for its picker", "[ui][slot]") {
    SlotUnderTest t;
    t.h.click("field");
    CHECK(t.clicks == 1);
}

TEST_CASE("A slot's clear button empties it, and does nothing while it is empty", "[ui][slot]") {
    SlotUnderTest t;
    t.h.click("clear");
    CHECK(t.clears == 0);

    t.held = "goblin";
    t.h.step();
    t.h.click("clear");
    CHECK(t.clears == 1);
    CHECK(t.held.empty());
}
