#include "kit_page.hpp"

#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/region.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"

#include <array>
#include <string>

namespace ftk::example {

namespace {

void heading(const char* label) {
    ui::FontScope bold(ui::Font::Bold, 0.0f);
    ui::text(label);
}

} // namespace

void KitPage::draw(glm::vec2 size, float top) {
    if (ui::begin_plain_window("##kit", {0.0f, top}, {size.x, size.y - top})) {
        heading("Buttons");
        if (ui::button("Save scene")) ++clicks_;
        ui::item_tooltip("Writes the scene to its file", "Ctrl S");
        ui::same_line();
        ui::button("Bake", ui::ButtonKind::Primary);
        ui::same_line();
        ui::button("Revert", ui::ButtonKind::Ghost);
        ui::same_line();
        ui::button("Delete", ui::ButtonKind::Danger);
        ui::same_line();
        ui::button("Remove", ui::ButtonKind::GhostDanger);
        ui::same_line();
        ui::small_button("Stop");
        {
            ui::Disabled off(disabled_);
            ui::button("Upload");
        }
        ui::same_line();
        if (ui::button(disabled_ ? "Enable Upload" : "Disable Upload")) disabled_ = !disabled_;
        ui::text("Saved " + std::to_string(clicks_) + " times", theme::text_secondary());
        ui::separator();

        heading("Text");
        ui::text("The kit's body text, in its own face at its size.");
        ui::text("A line in the secondary colour.", theme::text_secondary());
        ui::text("A line in the disabled colour.", theme::text_disabled());
        ui::bullet_text("textures/rock.png");
        ui::bullet_text("meshes/crate_large_weathered.fjmesh");
        {
            ui::FontScope mono(ui::Font::Mono, theme::SMALL_TEXT);
            ui::text("0x5EED  mono at the small size");
        }
        ui::separator();

        heading("Lines to pick, in a region that scrolls");
        if (auto log = ui::Region("##log", {360.0f, 160.0f}, {.border = true})) {
            static constexpr std::array<const char*, 6> FILES{"harbour.fjell", "crate.fjmesh", "rock.png",
                                                              "sea.fjsl", "hero.fjanimset", "fog.fjvfx"};
            for (int i = 0; i < 18; ++i) {
                ui::IdScope id(i);
                if (ui::selectable(FILES[static_cast<std::size_t>(i) % FILES.size()], picked_ == i)) picked_ = i;
            }
        }
    }
    ui::end_plain_window();

    // A panel of the kit's own, over the page: an inspector's rows.
    ui::next_window_size_once({420.0f, 400.0f});
    ui::next_window_pos({size.x - 460.0f, top + 40.0f});
    if (auto panel = ui::Panel(ui::icon::folder, "Inspector")) {
        ui::next_width(ui::FILL);
        (void)ui::search_field("##find", query_);
        const ui::RowFilter filter(query_);
        if (auto table = ui::PropertyTable("##crate")) {
            ui::row("Name", [&] { (void)ui::text_field("##name", name_); });
            ui::row("Mass", [&] { (void)ui::drag("##mass", mass_, {.speed = 0.1f, .min = 0.0f, .max = 500.0f, .unit = ui::Unit::Kilograms}); });
            ui::row("Roughness", "How much light the surface scatters", [&] {
                (void)ui::slider("##roughness", roughness_, 0.0f, 1.0f);
            });
            ui::row("Position", [&] { (void)ui::vec3("##position", position_, {.unit = ui::Unit::Metres}); });
            ui::mark_next_row(true);
            ui::row("Gravity", [&] { (void)ui::checkbox("##gravity", gravity_); });
            ui::row("Visible", [&] {
                (void)ui::on_off("##visible", visible_);
                ui::same_line();
                ui::tag("+1 shader", "Compiled in", "2 shaders");
            });
            ui::row("Detail", [&] { (void)ui::stepper("##lod", lod_, 0, 4); });
            ui::hint("The level of detail the crate is drawn at, from the closest.");
        }
        ui::button("Open");
        ui::same_line();
        ui::button("Duplicate", ui::ButtonKind::Ghost);
    }
}

} // namespace ftk::example
