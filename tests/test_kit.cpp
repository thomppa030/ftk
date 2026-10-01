#include "ftk/ui/kit/asset_kind.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/component_block.hpp"
#include "ftk/ui/kit/dialog.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/inset_group.hpp"
#include "ftk/ui/kit/key_cap.hpp"
#include "ftk/ui/kit/loading.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/overlay.hpp"
#include "ftk/ui/kit/pane.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/tabs.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"
#include "imgui_harness.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

using ftk::test::ImGuiHarness;
namespace ui = ftk::ui;

TEST_CASE("Every button kind reports its click", "[ui][kit]") {
    const ui::ButtonKind kinds[] = {ui::ButtonKind::Secondary, ui::ButtonKind::Primary,
                                    ui::ButtonKind::Ghost, ui::ButtonKind::Danger,
                                    ui::ButtonKind::GhostDanger};
    for (auto kind : kinds) {
        ImGuiHarness h;
        int clicks = 0;
        h.set_ui([&] {
            if (ui::button("Save", kind)) ++clicks;
            h.mark("button");
        });
        h.step(2);
        h.click("button");
        CHECK(clicks == 1);
    }
}

TEST_CASE("An action leads with its icon and is as tall as any button", "[ui][kit]") {
    ImGuiHarness h;
    int clicks = 0;
    h.set_ui([&] {
        ui::button("Create material");
        h.mark("button");
        if (ui::action("+", "Create material")) ++clicks;
        h.mark("action");
    });
    h.step(2);
    const float button_height = h.rect_max("button").y - h.rect_min("button").y;
    const float action_height = h.rect_max("action").y - h.rect_min("action").y;
    CHECK(action_height == button_height);
    // Wider than the same verb on a plain button by the icon and its gap.
    const float plain = h.rect_max("button").x - h.rect_min("button").x;
    const float with_icon = h.rect_max("action").x - h.rect_min("action").x;
    CHECK(with_icon >= plain + ImGui::CalcTextSize("+  ").x - 0.5f);
    h.click("action");
    CHECK(clicks == 1);
}

TEST_CASE("An icon button is a frame-height square", "[ui][kit]") {
    ImGuiHarness h;
    int clicks = 0;
    h.set_ui([&] {
        if (ui::icon_button("clear", "x", "Clear")) ++clicks;
        h.mark("icon");
    });
    h.step(2);
    const ImVec2 min = h.rect_min("icon");
    const ImVec2 max = h.rect_max("icon");
    CHECK(max.x - min.x == max.y - min.y);
    h.click("icon");
    CHECK(clicks == 1);
}

TEST_CASE("A toggle reports clicks and leaves the state to the caller", "[ui][kit]") {
    ImGuiHarness h;
    bool on = false;
    h.set_ui([&] {
        if (ui::toggle_button("snap", "M", on, "Snap to grid")) on = !on;
        h.mark("toggle");
    });
    h.step(2);
    h.click("toggle");
    CHECK(on);
    h.click("toggle");
    CHECK_FALSE(on);
}

TEST_CASE("A foldable section opens and closes and remembers", "[ui][kit]") {
    ImGuiHarness h;
    bool open = false;
    h.set_ui([&] {
        open = ui::section_foldable("Weather", nullptr, true);
        h.mark("heading");
    });
    h.step(2);
    CHECK(open);
    h.click("heading");
    CHECK_FALSE(open);
    h.step(3);
    CHECK_FALSE(open);
    h.click("heading");
    CHECK(open);
}

TEST_CASE("A foldable sub-heading starts the way it is asked to", "[ui][kit]") {
    ImGuiHarness h;
    bool open = true;
    h.set_ui([&] {
        open = ui::subheading_foldable("Lightning", false);
        h.mark("heading");
    });
    h.step(2);
    CHECK_FALSE(open);
    h.click("heading");
    CHECK(open);
}

TEST_CASE("A component block folds from its header and asks to be removed from its trash icon", "[ui][kit]") {
    ImGuiHarness h;
    bool open = false;
    bool remove = false;
    int removals = 0;
    h.set_ui([&] {
        remove = false;
        open = ui::component_block("Collider", ftk::theme::Category::Physics, remove);
        h.mark("trash");  // the last item is the trash icon
        if (remove) ++removals;
    });
    h.step(2);
    CHECK(open);
    const ImVec2 trash_min = h.rect_min("trash");
    const ImVec2 trash_max = h.rect_max("trash");
    const float y = (trash_min.y + trash_max.y) * 0.5f;

    // The header left of the trash icon folds the block.
    ImGuiIO& io = ImGui::GetIO();
    const auto click_at = [&](float x) {
        io.AddMousePosEvent(x, y);
        h.step();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        h.step();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        h.step();
    };
    click_at(trash_min.x - 40.0f);
    CHECK_FALSE(open);
    click_at(trash_min.x - 40.0f);
    CHECK(open);

    // The trash icon asks for removal and leaves the fold alone.
    h.click("trash");
    CHECK(removals == 1);
    CHECK(open);
}

TEST_CASE("An icon button centres an icon wider than its padding leaves room for", "[ui][kit]") {
    ImGuiHarness h;
    h.set_ui([&] {
        // A wide label stands in for an icon glyph: wider than the square
        // minus ImGui's frame padding on both sides.
        ui::icon_button("wide", "WW", "Wide");
        h.mark("icon");
    });
    h.step(2);
    const ImVec2 min = h.rect_min("icon");
    const ImVec2 max = h.rect_max("icon");
    REQUIRE(ImGui::CalcTextSize("WW").x > (max.x - min.x) - ImGui::GetStyle().FramePadding.x * 2.0f);

    // Around the button, the text is the only thing drawn from the font
    // atlas rather than its white pixel, so its vertices give where the
    // label landed.
    const ImVec2 white = ImGui::GetIO().Fonts->TexUvWhitePixel;
    float text_min = FLT_MAX;
    float text_max = -FLT_MAX;
    const ImDrawData* data = ImGui::GetDrawData();
    for (const ImDrawList* list : data->CmdLists) {
        for (const ImDrawVert& v : list->VtxBuffer) {
            if (v.uv.x == white.x && v.uv.y == white.y) continue;
            if (v.pos.y < min.y || v.pos.y > max.y) continue;
            if (v.pos.x < min.x - 16.0f || v.pos.x > max.x + 16.0f) continue;
            text_min = std::min(text_min, v.pos.x);
            text_max = std::max(text_max, v.pos.x);
        }
    }
    REQUIRE(text_min < text_max);
    const float text_centre = (text_min + text_max) * 0.5f;
    CHECK(std::abs(text_centre - (min.x + max.x) * 0.5f) <= 1.0f);
}

TEST_CASE("An empty state sits in the middle of the space and offers its action", "[ui][kit]") {
    ImGuiHarness h;
    int clicks = 0;
    ImVec2 area_min;
    ImVec2 area_max;
    h.set_ui([&] {
        area_min = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        area_max = {area_min.x + avail.x, area_min.y + avail.y};
        if (ui::empty_state("?", "Nothing selected", "Pick an object to edit it here", "Select all")) {
            ++clicks;
        }
        h.mark("action");
    });
    h.step(2);
    const ImVec2 min = h.rect_min("action");
    const ImVec2 max = h.rect_max("action");
    CHECK(std::abs((min.x + max.x) * 0.5f - (area_min.x + area_max.x) * 0.5f) <= 1.0f);
    // The block is centred vertically, so its last line is below the middle
    // but nowhere near the bottom.
    CHECK(max.y > (area_min.y + area_max.y) * 0.5f);
    CHECK(max.y < area_max.y - 100.0f);
    h.click("action");
    CHECK(clicks == 1);
}

TEST_CASE("An asset's kind comes from its extension, by domain", "[ui][kit]") {
    using ftk::theme::Category;
    CHECK(ui::asset_kind(".png").category == Category::Rendering);
    CHECK(std::string(ui::asset_kind(".png").noun) == "texture");
    CHECK(ui::asset_kind(".ttf").category == Category::Ui);
    CHECK(ui::asset_kind(".wav").category == Category::Audio);
    CHECK(std::string(ui::asset_kind(".xyz").noun) == "file");
}

TEST_CASE("A program's own file types show as it registered them", "[ui][kit]") {
    using ftk::theme::Category;
    ui::register_asset_kind(".kittest", {"kit test", ui::icon::file, Category::Logic});
    CHECK(std::string(ui::asset_kind(".kittest").noun) == "kit test");
    CHECK(ui::asset_kind(".kittest").category == Category::Logic);
}

TEST_CASE("A disabled icon button still shows its tooltip", "[ui][kit]") {
    ImGuiHarness h;
    h.set_ui([&] {
        ImGui::BeginDisabled(true);
        ui::icon_button("clear", "x", "Nothing to clear");
        ImGui::EndDisabled();
        h.mark("button");
    });
    h.step(2);
    const ImVec2 min = h.rect_min("button");
    const ImVec2 max = h.rect_max("button");
    ImGui::GetIO().AddMousePosEvent((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    // Past the tooltip delay.
    h.step(30);
    const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
    REQUIRE(tooltip != nullptr);
    CHECK(tooltip->WasActive);
}

TEST_CASE("A callout wraps its text, reports its action, and the layout goes on below it", "[ui][kit]") {
    ImGuiHarness h;
    int clicks = 0;
    h.set_ui([&] {
        ImGui::BeginChild("##narrow", {220.0f, 400.0f});
        if (ui::callout(ui::Severity::Error, "sea.fjsl failed to compile",
                        "Line 42: 'foam_bias' : undeclared identifier. It draws with the last "
                        "version that compiled.",
                        "+", "Open sea.fjsl")) {
            ++clicks;
        }
        h.mark("box");
        ImGui::Button("after");
        h.mark("after");
        ImGui::EndChild();
    });
    h.step(3);
    // The explanation wraps inside the 220 px window onto several lines:
    // the box is taller than a title, three lines and the button.
    const float line = ImGui::GetTextLineHeightWithSpacing();
    CHECK(h.rect_max("box").y - h.rect_min("box").y > line * 4.0f + ImGui::GetFrameHeight());
    CHECK(h.rect_min("after").y >= h.rect_max("box").y);
    // The action is the last thing in the box, under the text: press and
    // let go on it.
    const ImVec2 min = h.rect_min("box");
    const ImVec2 max = h.rect_max("box");
    const ImVec2 action{min.x + 60.0f, max.y - 8.0f - ImGui::GetFrameHeight() * 0.5f};
    h.drag(action, action);
    CHECK(clicks == 1);
}

TEST_CASE("A menu entry reports its click, and a disabled one doesn't", "[ui][kit]") {
    ImGuiHarness h;
    int fits = 0;
    int creates = 0;
    h.set_ui([&] {
        if (ui::menu_item({.label = "Fit to mesh"})) ++fits;
        h.mark("fit");
        if (ui::menu_item({.label = "Create material", .enabled = false,
                           .disabled_reason = "Needs a terrain asset first"})) {
            ++creates;
        }
        h.mark("create");
    });
    h.step(2);
    h.click("fit");
    h.click("create");
    CHECK(fits == 1);
    CHECK(creates == 0);
}

TEST_CASE("A tool button is a tool-sized square and reports its click", "[ui][kit]") {
    ImGuiHarness h;
    bool on = false;
    h.set_ui([&] {
        if (ui::tool_button("raise", "R", on, "Raise")) on = !on;
        h.mark("tool");
    });
    h.step(2);
    CHECK(h.rect_max("tool").x - h.rect_min("tool").x == ftk::theme::TOOL_BUTTON);
    CHECK(h.rect_max("tool").y - h.rect_min("tool").y == ftk::theme::TOOL_BUTTON);
    h.click("tool");
    CHECK(on);
}

TEST_CASE("An inset group folds from its heading and keeps its contents inside it", "[ui][kit]") {
    ImGuiHarness h;
    bool drawn = false;
    float window_right = 0.0f;
    h.set_ui([&] {
        window_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        drawn = false;
        const float heading_y = ImGui::GetCursorScreenPos().y;
        if (auto box = ui::InsetGroup("##box", "Material", nullptr, {1, 1, 1, 1})) {
            drawn = true;
            ImGui::SetNextItemWidth(-FLT_MIN);
            float v = 0.5f;
            (void)ui::drag("##v", v);
            h.mark("field");
        }
        (void)heading_y;
    });
    h.step(2);
    REQUIRE(drawn);
    // The field stops at the box's padding, not at the window's edge.
    CHECK(h.rect_max("field").x < window_right - 4.0f);
    // The heading is the box's first line: clicking it folds the box.
    const ImVec2 field_min = h.rect_min("field");
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(field_min.x + 40.0f, field_min.y - ImGui::GetFrameHeight() * 0.5f - 2.0f);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    h.step(2);
    CHECK_FALSE(drawn);
}

TEST_CASE("A mode button is as wide as mode_button_width says, to be placed by it", "[ui][kit]") {
    ImGuiHarness h;
    h.set_ui([&] {
        (void)ui::mode_button("##view", ui::icon::debug, "DDGI indirect", true);
        h.mark("view");
    });
    h.step(2);
    const float drawn = h.rect_max("view").x - h.rect_min("view").x;
    CHECK(drawn == ui::mode_button_width(ui::icon::debug, "DDGI indirect"));
    CHECK(drawn > ui::mode_button_width(ui::icon::debug, "Lit"));
}

TEST_CASE("A marked row's label makes room for the dot, marked or not, and only that row", "[ui][kit]") {
    ImGuiHarness h;
    float plain = 0.0f;
    float set_here = 0.0f;
    float not_set = 0.0f;
    float after_skipped = 0.0f;
    h.set_ui([&] {
        if (auto t = ui::PropertyTable("##rows")) {
            // In the value column the last item is still the row's label.
            ui::row("Plain", [&] { plain = ImGui::GetItemRectMin().x; });
            ui::mark_next_row(true);
            ui::row("Set here", [&] { set_here = ImGui::GetItemRectMin().x; });
            ui::mark_next_row(false);
            ui::row("Not set", [&] { not_set = ImGui::GetItemRectMin().x; });
            {
                // A search that leaves the marked row out doesn't hand its
                // mark to the next row.
                ui::RowFilter filter("Next");
                ui::mark_next_row(true);
                ui::row("Skipped", [] {});
                ui::row("Next", [&] { after_skipped = ImGui::GetItemRectMin().x; });
            }
        }
    });
    h.step(2);
    CHECK(set_here > plain);
    CHECK(not_set == set_here);
    CHECK(after_skipped == plain);
}

TEST_CASE("An on/off switch flips its value, each flip a finished edit", "[ui][kit]") {
    ImGuiHarness h;
    bool on = true;
    int commits = 0;
    h.set_ui([&] {
        if (ui::on_off("##switch", on).committed) ++commits;
        h.mark("switch");
    });
    h.step(2);
    h.click("switch");
    CHECK_FALSE(on);
    h.click("switch");
    CHECK(on);
    CHECK(commits == 2);
}

TEST_CASE("A block with a switch turns off without folding or removing", "[ui][kit]") {
    ImGuiHarness h;
    bool enabled = true;
    bool removed = false;
    bool open = false;
    ImVec2 header_max{};
    h.set_ui([&] {
        open = ui::component_block("Gravity", ftk::theme::Category::Vfx, removed, enabled);
        header_max = ImGui::GetItemRectMax();  // the trash icon, last in the header
    });
    h.step(2);
    // The switch sits just before the trash icon.
    const float h_frame = ImGui::GetFrameHeight();
    const ImVec2 at{header_max.x - h_frame - ftk::theme::GAP_S - 14.0f, header_max.y - h_frame * 0.5f};
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.x, at.y);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    h.step(2);
    CHECK_FALSE(enabled);
    CHECK(open);
    CHECK_FALSE(removed);
}

TEST_CASE("A tab strip reports only the user's picks, not a selection the caller moved", "[ui][kit]") {
    ImGuiHarness h;
    const std::string names[] = {"embers", "smoke", "sparks"};
    int selected = 0;
    int picks = 0;
    h.set_ui([&] {
        const auto tabs = ui::tab_strip("##emitters", {.names = names, .selected = selected, .add_label = "Add emitter"});
        if (tabs.selected) {
            ++picks;
            selected = *tabs.selected;
        }
    });
    h.step(3);
    // An emitter added or removed moves the selection from outside.
    selected = 2;
    h.step(4);
    CHECK(picks == 0);
    CHECK(selected == 2);
}

TEST_CASE("A key cap listens from its name and lists from its chevron", "[ui][kit]") {
    ImGuiHarness h;
    int listens = 0;
    int lists = 0;
    ImVec2 start{};
    float short_end = 0.0f;
    float long_end = 0.0f;
    float long_start = 0.0f;
    h.set_ui([&] {
        start = ImGui::GetCursorScreenPos();
        const auto cap = ui::key_cap({.id = "##w", .device_icon = ui::icon::keyboard, .label = "W"});
        h.mark("chevron");  // the last item: the chevron
        short_end = ImGui::GetItemRectMax().x;
        if (cap.listen) ++listens;
        if (cap.list) ++lists;
        long_start = ImGui::GetCursorScreenPos().x;
        (void)ui::key_cap({.id = "##shift", .device_icon = ui::icon::keyboard, .label = "Left Shift"});
        long_end = ImGui::GetItemRectMax().x;
    });
    h.step(2);
    h.click("chevron");
    CHECK(lists == 1);
    CHECK(listens == 0);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(start.x + 6.0f, start.y + ImGui::GetFrameHeight() * 0.5f);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    h.step(2);
    CHECK(listens == 1);
    CHECK(long_end - long_start > short_end - start.x);
}

TEST_CASE("A named box's remove icon removes without folding it", "[ui][kit]") {
    ImGuiHarness h;
    bool remove = false;
    bool open = false;
    float right = 0.0f;
    float top = 0.0f;
    h.set_ui([&] {
        top = ImGui::GetCursorScreenPos().y;
        right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        auto box = ui::InsetGroup("##jump", {.text = "jump", .named = true, .remove = &remove});
        open = static_cast<bool>(box);
    });
    h.step(2);
    // The remove icon: a frame-height square at the heading's right end.
    const float side = ImGui::GetFrameHeight();
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(right - side * 0.5f - 4.0f, top + 3.0f + side * 0.5f);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    h.step(2);
    CHECK(remove);
    CHECK(open);
}

TEST_CASE("A viewport pill sits inside the image's corner it is placed in", "[ui][kit]") {
    ImGuiHarness h;
    const ImVec2 image_min{20.0f, 30.0f};
    const ImVec2 image_max{420.0f, 330.0f};
    ImVec2 left_min{};
    float right_edge = 0.0f;
    h.set_ui([&] {
        {
            ui::ViewportPill pill("##left", image_min, image_max, ui::PillPlace::TopLeft);
            (void)ui::icon_button("##grid", ui::icon::grid, "Grid");
            left_min = ImGui::GetItemRectMin();
        }
        {
            ui::ViewportPill pill("##right", image_min, image_max, ui::PillPlace::TopRight);
            (void)ui::icon_button("##a", ui::icon::grid, "A");
            ImGui::SameLine();
            (void)ui::icon_button("##b", ui::icon::grid, "B");
            right_edge = ImGui::GetItemRectMax().x;
        }
    });
    // The right one learns its width in the first frame and is placed by it after.
    h.step(3);
    CHECK(left_min.x > image_min.x);
    CHECK(left_min.y > image_min.y);
    CHECK(right_edge < image_max.x);
    CHECK(right_edge > image_max.x - 16.0f);
}

TEST_CASE("A viewport window puts its picture at its very corner", "[ui][kit]") {
    ImGuiHarness h;
    ImVec2 window_pos{};
    ImVec2 first{};
    h.set_ui([&] {
        ImGui::SetNextWindowPos({50.0f, 60.0f});
        ImGui::SetNextWindowSize({200.0f, 150.0f});
        if (auto window = ui::ViewportWindow("##viewport", nullptr, ImGuiWindowFlags_NoTitleBar)) {
            window_pos = ImGui::GetWindowPos();
            first = ImGui::GetCursorScreenPos();
        }
    });
    h.step(2);
    CHECK(first.x == window_pos.x);
    CHECK(first.y == window_pos.y);
}

TEST_CASE("The context tab bar and a host window leave ImGui's stacks as they found them", "[ui][kit]") {
    ImGuiHarness h;
    int style_vars = -1;
    int colours = -1;
    h.set_ui([&] {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        const int vars_before = g.StyleVarStack.Size;
        const int colours_before = g.ColorStack.Size;
        {
            auto bar = ui::ContextTabBar("##context_bar");
            if (bar && ImGui::BeginTabBar("##tabs")) {
                if (ImGui::BeginTabItem("Scene")) ImGui::EndTabItem();
                ImGui::EndTabBar();
            }
        }
        (void)ui::begin_host_window("##host", ImGuiWindowFlags_NoTitleBar);
        ImGui::End();
        style_vars = g.StyleVarStack.Size - vars_before;
        colours = g.ColorStack.Size - colours_before;
    });
    h.step(3);
    CHECK(style_vars == 0);
    CHECK(colours == 0);
}

TEST_CASE("Each ImGui context keeps its own fonts", "[ui][kit]") {
    // Two windows, each with its own context and atlas: the second loading
    // its faces must not hand them to the first.
    ImGuiContext* editor = ImGui::CreateContext();
    ImGuiContext* browser = ImGui::CreateContext();
    ImFont editor_bold;
    ImFont browser_bold;

    ImGui::SetCurrentContext(editor);
    ftk::theme::detail::current_faces().bold = &editor_bold;
    ImGui::SetCurrentContext(browser);
    ftk::theme::detail::current_faces().bold = &browser_bold;

    ImGui::SetCurrentContext(editor);
    CHECK(ftk::theme::bold_font() == &editor_bold);
    ImGui::SetCurrentContext(browser);
    CHECK(ftk::theme::bold_font() == &browser_bold);

    ftk::theme::forget_fonts(browser);
    ftk::theme::forget_fonts(editor);
    ImGui::DestroyContext(browser);
    ImGui::DestroyContext(editor);
}

TEST_CASE("A pane and the loading marks leave ImGui's stacks as they found them", "[ui][kit]") {
    ImGuiHarness h;
    int style_vars = -1;
    int colours = -1;
    int fonts = -1;
    h.set_ui([&] {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        const int vars_before = g.StyleVarStack.Size;
        const int colours_before = g.ColorStack.Size;
        const int fonts_before = g.FontStack.Size;
        {
            auto side = ui::Pane("##side", {220.0f, 300.0f}, ui::PaneSurface::Sunken);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ui::spinner(dl, {110.0f, 200.0f}, 14.0f, 1.0f);
            ui::loading(dl, {110.0f, 230.0f}, "Opening harbour\xe2\x80\xa6", 1.0f);
        }
        style_vars = g.StyleVarStack.Size - vars_before;
        colours = g.ColorStack.Size - colours_before;
        fonts = g.FontStack.Size - fonts_before;
    });
    h.step(3);
    CHECK(style_vars == 0);
    CHECK(colours == 0);
    CHECK(fonts == 0);
}

TEST_CASE("A window's button bar confirms on Enter and cancels on Esc, unless something is typed", "[ui][kit]") {
    ImGuiHarness h;
    ui::DialogAnswer answer = ui::DialogAnswer::None;
    bool can_confirm = true;
    std::string typed;
    h.set_ui([&] {
        ImGui::InputText("##field", &typed);
        h.mark("field");
        const auto a = ui::window_bar({.title = "Import model", .confirm = "Import"}, {.can_confirm = can_confirm});
        if (a != ui::DialogAnswer::None) answer = a;
    });
    h.step(2);

    h.press(ImGuiKey_Enter);
    CHECK(answer == ui::DialogAnswer::Confirm);

    answer = ui::DialogAnswer::None;
    h.press(ImGuiKey_Escape);
    CHECK(answer == ui::DialogAnswer::Cancel);

    // Greyed out, Enter does nothing.
    answer = ui::DialogAnswer::None;
    can_confirm = false;
    h.press(ImGuiKey_Enter);
    CHECK(answer == ui::DialogAnswer::None);

    // Typing in a field, Enter and Esc are the field's.
    can_confirm = true;
    h.click("field");
    h.press(ImGuiKey_Enter);
    CHECK(answer == ui::DialogAnswer::None);
}

TEST_CASE("A corner note hangs from its corner, and a key makes its line taller", "[ui][kit]") {
    ImGuiHarness h;
    ImVec2 words{};
    ImVec2 with_key{};
    ImVec2 longer{};
    int vertices_before = 0;
    int vertices_after = 0;
    h.set_ui([&] {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ftk::ui::OverlayPiece plain[] = {{"releases input"}};
        const ftk::ui::OverlayPiece keyed[] = {{"Esc", true}, {"releases input"}};
        const ftk::ui::OverlayPiece more[] = {{"releases input, and more"}};
        vertices_before = dl->VtxBuffer.Size;
        words = ui::corner_note(dl, {400.0f, 300.0f}, ui::Corner::BottomRight,
                                {.line = plain, .ink = ftk::theme::text_secondary()});
        vertices_after = dl->VtxBuffer.Size;
        with_key = ui::corner_note(dl, {0.0f, 0.0f}, ui::Corner::TopLeft,
                                   {.line = keyed, .ink = ftk::theme::text_secondary()});
        longer = ui::corner_note(dl, {0.0f, 0.0f}, ui::Corner::TopLeft,
                                 {.line = more, .ink = ftk::theme::text_secondary()});
    });
    h.step(2);
    CHECK(vertices_after > vertices_before);
    CHECK(with_key.y > words.y);
    CHECK(with_key.x > words.x);
    CHECK(longer.x > words.x);
    CHECK(longer.y == words.y);
}
