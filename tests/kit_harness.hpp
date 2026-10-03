#pragma once

// The harness the kit's tests drive the kit with. Both take the kit's own
// types and read the same, so a kit test is written once: KitHarness is
// ImGuiHarness while the tests run against the kit's ImGui implementation,
// and UiHarness when they are built against the core's (FTK_KIT_ON_CORE,
// Phase 6 of the UI core plan). On the core it gives the kit its faces and
// draws the UI in a plain window of the kit's filling the display, as
// ImGuiHarness draws it in one of ImGui's.

#ifdef FTK_KIT_ON_CORE
#include "ftk/test/ui_harness.hpp"
#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/kit/item.hpp"
#else
#include "ftk/test/imgui_harness.hpp"
#endif

#include <functional>
#include <string>
#include <utility>

namespace ftk::test {

#ifdef FTK_KIT_ON_CORE
class KitHarness : public UiHarness {
public:
    explicit KitHarness(glm::vec2 size = {800.0f, 600.0f}) : UiHarness(size, 1.0f, false) {
        ui::use_kit_fonts(ui(), FTK_FONTS_DIR);
    }

    void set_ui(std::function<void()> draw) {
        UiHarness::set_ui([this, draw = std::move(draw)] {
            if (ui::begin_plain_window("##harness", {0.0f, 0.0f}, size()) && draw) draw();
            ui::end_plain_window();
        });
    }

    /// Remembers the kit's item just drawn, as ImGuiHarness remembers
    /// ImGui's: a piece may place an item by hand, past the layout.
    void mark(const std::string& name) { UiHarness::mark(name, {.min = ui::item_min(), .max = ui::item_max()}); }
};
#else
using KitHarness = ImGuiHarness;
#endif

} // namespace ftk::test
