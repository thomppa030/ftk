// The kit's tests that check what it does to Dear ImGui itself: its window
// flags, its contexts, its draw data and its internal state. They run only
// against the kit's ImGui implementation, and go with it at the flip (Phase
// 9 of the UI core plan). Every other kit test reads the same against
// either implementation (kit_harness.hpp).

#include "ftk/test/imgui_harness.hpp"
#include "ftk/ui/imgui_theme.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/loading.hpp"
#include "ftk/ui/kit/overlay.hpp"
#include "ftk/ui/kit/pane.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/settings_file.hpp"
#include "ftk/ui/kit/tabs.hpp"
#include "ftk/ui/kit/tree.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

using ftk::test::ImGuiHarness;
namespace ui = ftk::ui;
namespace theme = ftk::theme;

TEST_CASE("An icon button centres an icon wider than its padding leaves room for", "[ui][kit]") {
    ImGuiHarness h;
    h.set_ui([&] {
        // A wide label stands in for an icon glyph: wider than the square
        // minus ImGui's frame padding on both sides.
        ui::icon_button("wide", "WW", "Wide");
        h.mark("icon");
    });
    h.step(2);
    const glm::vec2 min = h.rect("icon").min;
    const glm::vec2 max = h.rect("icon").max;
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

TEST_CASE("A texture holds the ImGui id it is made from, and none is zero", "[ui][kit]") {
    const ImTextureID imgui_id = 0x5EED;
    const ui::Texture texture = imgui_id;
    CHECK(texture.id == 0x5EED);
    CHECK(static_cast<bool>(texture));
    CHECK_FALSE(static_cast<bool>(ui::Texture{}));
    CHECK(texture == ui::Texture{0x5EED});
    CHECK(texture != ui::Texture{});
}

TEST_CASE("A window leaves out what its options say and nothing else", "[ui][kit]") {
    using ui::detail::imgui_window_flags;
    CHECK(imgui_window_flags({}) == ImGuiWindowFlags_None);
    CHECK(imgui_window_flags({.title_bar = false}) == ImGuiWindowFlags_NoTitleBar);
    CHECK(imgui_window_flags({.resize = false}) == ImGuiWindowFlags_NoResize);
    CHECK(imgui_window_flags({.move = false}) == ImGuiWindowFlags_NoMove);
    CHECK(imgui_window_flags({.collapse = false}) == ImGuiWindowFlags_NoCollapse);
    CHECK(imgui_window_flags({.scrollbar = false}) == ImGuiWindowFlags_NoScrollbar);
    CHECK(imgui_window_flags({.background = false}) == ImGuiWindowFlags_NoBackground);
    CHECK(imgui_window_flags({.docking = false}) == ImGuiWindowFlags_NoDocking);
    CHECK(imgui_window_flags({.to_front = false}) == ImGuiWindowFlags_NoBringToFrontOnFocus);
    CHECK(imgui_window_flags({.nav_focus = false}) == ImGuiWindowFlags_NoNavFocus);
    CHECK(imgui_window_flags({.remembered = false}) == ImGuiWindowFlags_NoSavedSettings);
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
        (void)ui::begin_host_window("##host");
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
    CHECK(ftk::theme::imgui_font(ftk::theme::bold_font()) == &editor_bold);
    ImGui::SetCurrentContext(browser);
    CHECK(ftk::theme::imgui_font(ftk::theme::bold_font()) == &browser_bold);

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

TEST_CASE("A corner note draws into ImGui's list it is given", "[ui][kit]") {
    ImGuiHarness h;
    int vertices_before = 0;
    int vertices_after = 0;
    h.set_ui([&] {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ui::OverlayPiece plain[] = {{"releases input"}};
        vertices_before = dl->VtxBuffer.Size;
        (void)ui::corner_note(dl, {400.0f, 300.0f}, ui::Corner::BottomRight,
                              {.line = plain, .ink = theme::text_secondary()});
        vertices_after = dl->VtxBuffer.Size;
    });
    h.step(2);
    CHECK(vertices_after > vertices_before);
}

TEST_CASE("An id scope gives the ids ImGui's own push gave", "[ui][kit]") {
    // What ImGui keeps by id (a fold, a column's width) is found again once
    // a panel moves from PushID to the kit's scope.
    ImGuiHarness h;
    std::vector<std::pair<ImGuiID, ImGuiID>> ids;
    h.set_ui([&] {
        ids.clear();
        // The kit's scope over `scoped`, ImGui's push of `pushed`.
        const auto pair = [&](auto scoped, auto pushed) {
            ImGuiID in_scope = 0;
            {
                ui::IdScope id(scoped);
                in_scope = ImGui::GetID("##row");
            }
            ImGui::PushID(pushed);
            ids.emplace_back(in_scope, ImGui::GetID("##row"));
            ImGui::PopID();
        };
        pair("Layer", "Layer");
        pair(std::string_view("Layer"), "Layer");
        pair(std::string("Layer"), "Layer");
        pair(7, 7);
        pair(static_cast<const void*>(&ids), static_cast<const void*>(&ids));
    });
    h.step();
    REQUIRE(ids.size() == 5);
    for (const auto& [scoped, pushed] : ids) CHECK(scoped == pushed);
}

TEST_CASE("A kit id is ImGui's id for the same label", "[ui][kit]") {
    ImGuiHarness h;
    bool same_id = false;
    h.set_ui([&] { same_id = ui::id("##picked") == ImGui::GetID("##picked"); });
    h.step();
    CHECK(same_id);
}

TEST_CASE("The style's spacing and padding are the kit's metrics", "[ui][kit]") {
    ImGuiHarness h;
    h.set_ui([&] {
        CHECK(ui::item_spacing() == glm::vec2(ImGui::GetStyle().ItemSpacing));
        CHECK(ui::item_inner_spacing() == glm::vec2(ImGui::GetStyle().ItemInnerSpacing));
        CHECK(ui::frame_padding() == glm::vec2(ImGui::GetStyle().FramePadding));
        CHECK(ui::window_padding() == glm::vec2(ImGui::GetStyle().WindowPadding));
    });
    h.step();
}

TEST_CASE("A window's first size is only its first", "[ui][kit]") {
    ImGuiHarness h;
    glm::vec2 size{};
    h.set_ui([&] {
        ui::next_window_size_once({320.0f, 240.0f});
        ImGui::Begin("Preview");
        size = ui::window_size();
        ImGui::End();
    });
    h.step(2);
    CHECK(size == glm::vec2{320.0f, 240.0f});
    ImGui::SetWindowSize("Preview", {400.0f, 300.0f});
    h.step(2);
    CHECK(size == glm::vec2{400.0f, 300.0f});
}

TEST_CASE("A settings section is read back from the settings file and written to it", "[ui][kit]") {
    ImGuiHarness h;
    int text_size = 14;
    std::vector<std::string> read;
    ui::add_settings_section({
        .name = "KitView",
        .read = [&](std::string_view entry, std::string_view line) {
            read.push_back(std::string(entry) + ":" + std::string(line));
            if (entry == "Editor" && line.starts_with("TextSize=")) text_size = std::stoi(std::string(line.substr(9)));
        },
        .write = [&] {
            return std::vector<ui::SettingsEntry>{{"Editor", {"TextSize=" + std::to_string(text_size)}}};
        },
    });
    ImGui::LoadIniSettingsFromMemory("[KitView][Editor]\nTextSize=17\n\n[KitView][Other]\nTextSize=9\n");
    CHECK(text_size == 17);
    CHECK(read == std::vector<std::string>{"Editor:TextSize=17", "Other:TextSize=9"});

    text_size = 12;
    ui::settings_changed();
    const std::string saved = ImGui::SaveIniSettingsToMemory();
    CHECK(saved.find("[KitView][Editor]\nTextSize=12\n") != std::string::npos);

    // The kit keeps a section for the program's life; these callbacks don't.
    ui::add_settings_section({.name = "KitView"});
}

TEST_CASE("Ctrl F reaches the search field without a navigation ring around it", "[ui][kit]") {
    ImGuiHarness h;
    std::string query;
    h.set_ui([&] {
        ui::next_width(200.0f);
        (void)ui::search_field("##search", query);
    });
    h.step(2);
    h.press(ui::Key::F, {.ctrl = true});
    h.step(2);
    REQUIRE(ImGui::GetCurrentContext()->ActiveId != 0);
    CHECK_FALSE(ImGui::GetCurrentContext()->NavCursorVisible);
}

TEST_CASE("A rename started from the keyboard draws no navigation ring", "[ui][kit]") {
    // The editor navigates with the keyboard, which is what turns the ring on.
    ImGuiHarness h;
    ui::RenameBox rename;
    h.set_ui([&] {
        if (auto tree = ui::Tree("##tree")) {
            (void)ui::tree_row({.id = "1", .name = "Crate", .rename = &rename, .key = 1});
            h.mark("row");
        }
    });
    h.step(2);
    h.click("row");
    h.press(ui::Key::F2);
    rename.start(1, "Crate");
    h.step(3);
    REQUIRE(ImGui::GetCurrentContext()->ActiveId != 0);
    CHECK_FALSE(ImGui::GetCurrentContext()->NavCursorVisible);
}

namespace {

// Where the glyphs drawn in `colour` lie across the band of `min`..`max`:
// the font atlas's vertices, not the white pixel's.
std::pair<float, float> glyphs_across(glm::vec2 min, glm::vec2 max, ImU32 colour) {
    const ImVec2 white = ImGui::GetIO().Fonts->TexUvWhitePixel;
    float lo = FLT_MAX;
    float hi = -FLT_MAX;
    for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists) {
        for (const ImDrawVert& v : list->VtxBuffer) {
            if ((v.uv.x == white.x && v.uv.y == white.y) || v.col != colour) continue;
            if (v.pos.y < min.y || v.pos.y > max.y) continue;
            lo = std::min(lo, v.pos.x);
            hi = std::max(hi, v.pos.x);
        }
    }
    return {lo, hi};
}

} // namespace

TEST_CASE("A row's tag sits at its end in the quiet colour, and a long name stops before it", "[ui][kit]") {
    ImGuiHarness h;
    h.set_ui([&] {
        // As narrow as a docked hierarchy gets.
        ImGui::BeginChild("##panel", {240.0f, 120.0f});
        if (auto tree = ui::Tree("##tree")) {
            (void)ui::tree_row({.id = "1", .name = "managers", .tag = "persistent"});
            h.mark("short");
            (void)ui::tree_row({.id = "2", .name = "the far side of the northern harbour", .tag = "persistent"});
            h.mark("long");
        }
        ImGui::EndChild();
    });
    h.step(2);

    const ImU32 quiet = ImGui::GetColorU32(theme::text_disabled());
    const ImU32 name = ImGui::GetColorU32(theme::text());
    for (const char* row : {"short", "long"}) {
        INFO(row);
        const glm::vec2 min = h.rect(row).min;
        const glm::vec2 max = h.rect(row).max;
        const auto [tag_lo, tag_hi] = glyphs_across(min, max, quiet);
        const auto [name_lo, name_hi] = glyphs_across(min, max, name);
        REQUIRE(tag_lo < tag_hi);
        REQUIRE(name_lo < name_hi);
        // At the row's right end, inside it.
        CHECK(tag_hi <= max.x);
        CHECK(tag_hi > max.x - 16.0f);
        CHECK(name_hi < tag_lo);
    }
}
