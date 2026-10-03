#include "kit_page.hpp"

#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/color_field.hpp"
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

constexpr ui::Choice<KitPage::Body> BODIES[] = {
    {KitPage::Body::Static, "Static"}, {KitPage::Body::Dynamic, "Dynamic"}, {KitPage::Body::Kinematic, "Kinematic"},
};
constexpr ui::Choice<KitPage::Shape> SHAPES[] = {
    {KitPage::Shape::Box, "Box"},
    {KitPage::Shape::Sphere, "Sphere"},
    {KitPage::Shape::Capsule, "Capsule"},
    {KitPage::Shape::Mesh, "Mesh"},
};
constexpr std::array<const char*, 3> EVENTS{"Storm rolls in", "Fog at dusk", "Harbour bells"};
constexpr std::array<glm::vec3, 6> PALETTE{{
    {0.83f, 0.63f, 0.31f},
    {0.78f, 0.33f, 0.29f},
    {0.36f, 0.62f, 0.42f},
    {0.30f, 0.52f, 0.78f},
    {0.58f, 0.44f, 0.76f},
    {0.86f, 0.86f, 0.84f},
}};

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
        ui::same_line(32.0f);
        {
            // A colour picked in place, and swatches beneath it to pick.
            ui::Group column;
            ui::next_width(240.0f);
            (void)ui::color_picker("##paint", paint_);
            for (std::size_t i = 0; i < PALETTE.size(); ++i) {
                ui::IdScope id(static_cast<int>(i));
                if (i > 0) ui::same_line(4.0f);
                if (ui::swatch("##swatch", {PALETTE[i], 1.0f}, {24.0f, 24.0f})) paint_ = PALETTE[i];
            }
        }
    }
    ui::end_plain_window();

    // A panel of the kit's own, over the page: an inspector's rows.
    ui::next_window_size_once({420.0f, 560.0f});
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
            ui::row("Body", [&] { (void)ui::choice("##body", body_, BODIES); });
            ui::row("Collider", [&] { (void)ui::choice("##shape", shape_, SHAPES); });
            ui::row("Light", [&] { (void)ui::color("##light", light_, ui::ColorSpace::Linear); });
            ui::row("Tint", [&] { (void)ui::color("##tint", tint_, ui::ColorSpace::Srgb); });
            ui::row("On hit", [&] {
                const char* preview = event_ >= 0 ? EVENTS[static_cast<std::size_t>(event_)] : "Pick an event...";
                if (auto list = ui::Dropdown("##event", preview)) {
                    for (std::size_t i = 0; i < EVENTS.size(); ++i) {
                        if (ui::selectable(EVENTS[i], event_ == static_cast<int>(i))) event_ = static_cast<int>(i);
                    }
                }
            });
        }
        ui::button("Open");
        ui::same_line();
        ui::button("Duplicate", ui::ButtonKind::Ghost);
    }
}

} // namespace ftk::example
