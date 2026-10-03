#include "shell_page.hpp"

#include "icons.hpp"

#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/dock.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"

#include <array>
#include <utility>
#include <vector>

namespace ftk::example {

namespace {

constexpr std::array<const char*, 5> THINGS{"Harbour", "Sun", "Main Camera", "Crate", "Fog Bank"};

} // namespace

void ShellPage::draw(glm::vec2 size, float top) {
    using N = ui::DockNode;
    if (ui::begin_plain_window("##shell", {0.0f, top}, {size.x, size.y - top})) {
        if (ui::button("Reset layout")) built_ = false;
        ui::same_line();
        // A menu as data, as the editor's menu bar is.
        if (ui::button("Panels")) ui::open_popup("##panels");
        if (auto menu = ui::Popup("##panels")) {
            std::vector<ui::MenuEntry> forward;
            for (const char* name : {"Hierarchy", "Viewport", "Inspector", "Console", "History"}) {
                forward.push_back({.label = name, .chosen = [name] { ui::focus_window(name); }});
            }
            const std::vector<ui::MenuEntry> entries{
                ui::MenuEntry::heading("Layout"),
                {.label = "Reset layout", .chosen = [this] { built_ = false; }},
                ui::MenuEntry::submenu(nullptr, "Bring forward", std::move(forward)),
                ui::MenuEntry::separator(),
                ui::MenuEntry::note("Tear a tab out to float its panel"),
            };
            ui::menu_entries(entries);
        }
        ui::same_line();
        ui::align_text_to_frame();
        ui::text("Drag a splitter, a tab along its row or off it, or a floating window by its title onto a "
                 "leaf's tabs.",
                 theme::text_secondary());
        const ui::Id dock = ui::id("##shell_dock");
        ui::dockspace(dock);
        if (!built_) {
            ui::build_dock_tree(dock, ui::work_area().size,
                                N::left_right(0.2f, N::tabs({"Hierarchy"}),
                                              N::left_right(0.72f,
                                                            N::top_bottom(0.7f, N::tabs({"Viewport"}),
                                                                          N::tabs({"Console", "History"})),
                                                            N::tabs({"Inspector"}))));
            built_ = true;
        }
    }
    ui::end_plain_window();

    if (auto panel = ui::Panel(icon::hierarchy, "Hierarchy")) {
        for (int i = 0; i < static_cast<int>(THINGS.size()); ++i) {
            ui::IdScope scope(i);
            if (ui::selectable(THINGS[static_cast<std::size_t>(i)], i == selected_)) selected_ = i;
        }
    }
    if (auto panel = ui::Panel(icon::viewport, "Viewport")) {
        ui::empty_state(icon::scene, "The scene draws here", "Docked windows keep their place around it");
    }
    if (auto panel = ui::Panel(icon::inspector, "Inspector")) {
        ui::text(THINGS[static_cast<std::size_t>(selected_)]);
        ui::text("Selected in the hierarchy.", theme::text_secondary());
    }
    if (auto panel = ui::Panel(ui::icon::console, "Console")) {
        for (const std::string& line : lines_) ui::text(line);
    }
    if (auto panel = ui::Panel(ui::icon::history, "History")) {
        ui::text("Move Crate", theme::text_secondary());
        ui::text("Rename Barrel to Crate");
    }
}

} // namespace ftk::example
