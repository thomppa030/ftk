#include "kit_page.hpp"

#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/kit/asset_tile.hpp"
#include "ftk/ui/kit/bit_strip.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/color_field.hpp"
#include "ftk/ui/kit/curve_editor.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/inset_group.hpp"
#include "ftk/ui/kit/key_cap.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/list_editor.hpp"
#include "ftk/ui/kit/loading.hpp"
#include "ftk/ui/kit/overlay.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/plot.hpp"
#include "ftk/ui/kit/rail.hpp"
#include "ftk/ui/kit/region.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/save_bar.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/status_bar.hpp"
#include "ftk/ui/kit/table.hpp"
#include "ftk/ui/kit/tabs.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/tree.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
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
constexpr std::array<const char*, 8> LAYERS{"Default", "Props", "Water", "Terrain", "", "Sky", "Debris", ""};

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

    draw_emitters({10.0f, top + 570.0f});
    draw_feedback({size.x - 460.0f, top + 620.0f});
    draw_scene({10.0f, top + 1150.0f});
    draw_canvases({10.0f, top + 1610.0f});
}

void KitPage::draw_canvases(glm::vec2 at) {
    ui::next_window_size_once({1240.0f, 380.0f});
    ui::next_window_pos(at);
    auto panel = ui::Panel(ui::icon::history, "Canvases");
    if (!panel) return;

    {
        ui::Group left;
        (void)ui::curve_editor("##fade", fade_, {.size = {380.0f, 180.0f}, .hue = theme::Hue::orange});
        // Frame times, a ring of them read from the oldest.
        static constexpr std::array<float, 24> FRAMES{7.1f, 6.9f, 7.4f, 8.2f, 7.0f, 6.8f, 12.5f, 7.2f,
                                                      7.0f, 6.9f, 7.3f, 7.1f, 9.8f, 7.0f, 6.9f, 7.2f,
                                                      7.4f, 7.1f, 6.8f, 7.0f, 7.6f, 7.2f, 6.9f, 7.0f};
        ui::plot_lines("##frames", FRAMES, 5, {380.0f, 48.0f}, {.max = 16.6f, .overlay = "6.94 ms"});
    }
    ui::same_line(16.0f);

    // Three nodes, one value of each linked into a material.
    ui::NodeGraphDesc desc;
    ui::NodeRow strength;
    strength.output = ui::NodePin{.id = 11, .label = "Strength", .colour = theme::hue(theme::Hue::teal)};
    ui::NodeRow strength_value;
    strength_value.value = ui::NodeValue::number(strength_, {.speed = 0.01f, .lo = 0.0f, .hi = 1.0f});
    desc.nodes.push_back({.id = 1, .position = node_places_[0], .title = "Noise",
                          .hue = theme::hue(theme::Hue::teal), .rows = {strength_value, strength}});
    ui::NodeRow colour;
    colour.output = ui::NodePin{.id = 21, .label = "Colour", .colour = theme::hue(theme::Hue::coral)};
    ui::NodeRow colour_value;
    colour_value.value = ui::NodeValue::colour(tint_node_);
    desc.nodes.push_back({.id = 2, .position = node_places_[1], .title = "Tint",
                          .hue = theme::hue(theme::Hue::coral), .rows = {colour_value, colour}});
    ui::NodeRow mask;
    mask.input = ui::NodePin{.id = 31, .label = "Mask", .colour = theme::hue(theme::Hue::teal)};
    ui::NodeRow base;
    base.input = ui::NodePin{.id = 32, .label = "Base colour", .colour = theme::hue(theme::Hue::coral)};
    desc.nodes.push_back({.id = 3, .position = node_places_[2], .title = "Material",
                          .hue = theme::hue(theme::Hue::ochre), .rows = {mask, base}});
    desc.links = {{.id = 1, .from = {1, 11}, .to = {3, 31}}, {.id = 2, .from = {2, 21}, .to = {3, 32}}};
    const ui::NodeGraphEvents events = graph_.draw(desc);
    if (events.moved) node_places_[events.moved->node - 1] = events.moved->position;
}

void KitPage::draw_scene(glm::vec2 at) {
    ui::next_window_size_once({1240.0f, 440.0f});
    ui::next_window_pos(at);
    auto panel = ui::Panel(ui::icon::folder, "Scene");
    if (!panel) return;

    // A hierarchy in a box of its own.
    if (auto box = ui::Region("##hierarchy", {300.0f, 360.0f}, {.border = true})) {
        if (auto tree = ui::Tree("##scene")) {
            static constexpr std::array<theme::Hue, 2> COMPOSED{theme::Hue::coral, theme::Hue::teal};
            const auto row = [&](int id, const char* name, int depth, bool children, const char* icon,
                                 std::optional<theme::Hue> hue, std::span<const theme::Hue> dots = {}) {
                const std::string key = std::to_string(id);
                const auto drawn = ui::tree_row({.id = key.c_str(),
                                                 .name = name,
                                                 .depth = depth,
                                                 .has_children = children,
                                                 .icon = icon,
                                                 .hue = hue,
                                                 .dots = dots,
                                                 .selection = selected_node_ == id ? ui::RowSelection::Primary
                                                                                   : ui::RowSelection::None});
                if (drawn.clicked) selected_node_ = id;
                return drawn.open;
            };
            if (row(1, "Harbour", 0, true, ui::icon::project, theme::Hue::ochre)) {
                if (row(2, "Docks", 1, true, ui::icon::folder, theme::Hue::blue, COMPOSED)) {
                    (void)row(3, "crate_large_weathered", 2, false, ui::icon::file, std::nullopt);
                    (void)row(4, "Rope coil", 2, false, ui::icon::file, std::nullopt);
                }
                (void)row(5, "Sea", 1, false, ui::icon::texture, theme::Hue::cyan, COMPOSED);
                (void)row(6, "Sky with a name long enough to be cut short", 1, false, ui::icon::texture,
                          theme::Hue::periwinkle);
            }
        }
    }
    ui::same_line(16.0f);
    {
        ui::Group right;
        static constexpr ui::Column COLUMNS[] = {
            {.name = "Name", .default_sort = true},
            {.name = "Size", .sizing = ui::ColumnWidth::Fixed, .width = 80.0f},
            {.name = "Kind", .sizing = ui::ColumnWidth::Fixed, .width = 90.0f},
        };
        static constexpr std::array<std::array<const char*, 3>, 3> FILES{{
            {"harbour.fjell", "84 KB", "scene"},
            {"crate.fjmesh", "1.2 MB", "mesh"},
            {"sea.fjsl", "6 KB", "shader"},
        }};
        ui::next_width(520.0f);
        if (auto box = ui::Region("##files", {520.0f, 110.0f})) {
            if (auto table = ui::Table("##files", COLUMNS, {.sortable = true, .column_lines = true})) {
                ui::table_headers();
                for (const auto& file : FILES) {
                    ui::table_next_row();
                    for (const char* cell : file) {
                        ui::table_next_column();
                        ui::text(cell);
                    }
                }
            }
        }
        for (int i = 0; i < 3; ++i) {
            static constexpr std::array<const char*, 3> NAMES{"crate_large_weathered.fjmesh", "rock.png", "harbour.fjell"};
            static constexpr std::array<theme::Hue, 3> HUES{theme::Hue::blue, theme::Hue::coral, theme::Hue::ochre};
            if (i > 0) ui::same_line();
            const std::string key = std::to_string(i);
            if (ui::asset_tile({.id = key.c_str(),
                                .name = NAMES[static_cast<std::size_t>(i)],
                                .icon = ui::icon::file,
                                .hue = HUES[static_cast<std::size_t>(i)],
                                .selected = selected_tile_ == i})
                    .clicked) {
                selected_tile_ = i;
            }
        }
    }
    ui::same_line(16.0f);
    {
        ui::Group cards;
        ui::next_width(360.0f);
        (void)ui::list_editor("##points", points_, "Add point", "No points. The collider's corners are used",
                              [](glm::vec3& point, std::size_t) {
                                  return ui::vec3("##point", point, {.unit = ui::Unit::Metres});
                              });
    }
}

void KitPage::draw_feedback(glm::vec2 at) {
    ui::next_window_size_once({420.0f, 600.0f});
    ui::next_window_pos(at);
    if (auto panel = ui::Panel(ui::icon::warning, "Feedback")) {
        ui::status(ui::Severity::Success, "Baked 4 probes in 1.2 s");
        if (ui::status(ui::Severity::Warning, "leaf_colour is no longer in foliage.fjsl", ui::icon::remove, "Remove")) {
            ++clicks_;
        }
        (void)ui::callout(ui::Severity::Error, "sea.fjsl failed to compile",
                          "Line 42: 'foam_bias' is not declared. The sea draws with the last version that compiled.",
                          ui::icon::file, "Open sea.fjsl");
        ui::progress_bar(0.62f, -1.0f, "Baking lightmaps");
        const auto cap = ui::key_cap({.id = "##jump", .device_icon = ui::icon::file, .label = "Space",
                                      .listening = listening_});
        if (cap.listen) listening_ = !listening_;
        ui::same_line();
        (void)ui::key_cap({.id = "##crouch", .device_icon = ui::icon::file, .label = "Left Ctrl"});

        // A picture with notes over it and a pill of view buttons.
        const glm::vec2 min = ui::cursor();
        const glm::vec2 max = min + glm::vec2{ui::space_left().x, 110.0f};
        ui::DrawList dl = ui::DrawList::window();
        dl.rect_gradient(min, max, {0.20f, 0.33f, 0.45f, 1.0f}, {0.55f, 0.48f, 0.38f, 1.0f});
        {
            ui::ViewportPill pill("##view", min, max, ui::PillPlace::TopRight);
            (void)ui::icon_button("##grid", ui::icon::search, "Grid");
            ui::same_line();
            (void)ui::toggle_button("##wire", ui::icon::project, true, "Wireframe");
        }
        const ftk::ui::OverlayPiece showing[] = {{"Showing Wireframe"}};
        (void)ui::corner_note(dl, min + glm::vec2{ui::OVERLAY_INSET}, ui::Corner::TopLeft,
                              {.icon = ui::icon::warning, .line = showing, .ink = theme::warning()});
        const ftk::ui::OverlayPiece hint[] = {{"Shift", true}, {"Esc", true}, {"releases input"}};
        (void)ui::corner_note(dl, {min.x + ui::OVERLAY_INSET, max.y - ui::OVERLAY_INSET}, ui::Corner::BottomLeft,
                              {.line = hint, .ink = theme::text_secondary()});
        ui::loading(dl, {max.x - 60.0f, max.y - 70.0f}, "Opening", 0.4f);
        ui::set_cursor({min.x, max.y});
        ui::reserve({max.x - min.x, 0.0f});

        if (auto empty = ui::Region("##empty", {0.0f, 120.0f}, {.border = true})) {
            (void)ui::empty_state(ui::icon::folder, "Nothing selected", "Pick an object to edit it here",
                                  "Select all");
        }
        const ui::SaveAction saved = ui::save_bar(unsaved_, "settings.json");
        if (saved != ui::SaveAction::None) unsaved_ = false;
    }

    if (auto bar = ui::StatusBar()) {
        if (ui::status_button(ui::icon::folder, "Content", "Ctrl Space", content_open_)) content_open_ = !content_open_;
        ui::status_rule();
        ui::status_item(ui::icon::file, "harbour", unsaved_);
        ui::status_pill(ui::icon::success, "Playing", ui::StatusTone::Success);
        bar.right();
        ui::status_item(nullptr, "6.94 ms");
    }
}

void KitPage::draw_emitters(glm::vec2 at) {
    ui::next_window_size_once({780.0f, 560.0f});
    ui::next_window_pos(at);
    auto panel = ui::Panel(ui::icon::project, "Emitters");
    if (!panel) return;

    const auto tabs = ui::tab_strip("##emitters", {.names = emitters_,
                                                   .selected = emitter_,
                                                   .hue = theme::Hue::orange,
                                                   .add_label = "Add emitter",
                                                   .remove_label = "Remove emitter"});
    if (tabs.selected) emitter_ = *tabs.selected;
    if (tabs.add) emitters_.push_back("emitter " + std::to_string(emitters_.size() + 1));
    if (tabs.removed) {
        emitters_.erase(emitters_.begin() + *tabs.removed);
        emitter_ = std::min(emitter_, static_cast<int>(emitters_.size()) - 1);
    }

    ui::section("Spawn", ui::icon::history);
    if (auto table = ui::PropertyTable("##spawn")) {
        ui::row("Rate", [&] { (void)ui::drag("##rate", rate_, {.speed = 0.5f, .min = 0.0f, .max = 1000.0f}); });
        ui::row("Lifetime", [&] {
            (void)ui::drag("##lifetime", lifetime_, {.speed = 0.01f, .min = 0.0f, .max = 30.0f, .unit = ui::Unit::Seconds});
        });
    }
    if (auto box = ui::InsetGroup("##material", "Material · ember", ui::icon::texture, theme::hue(theme::Hue::orange))) {
        if (auto table = ui::PropertyTable("##material_rows")) {
            ui::row("Glow", [&] { (void)ui::slider("##glow", roughness_, 0.0f, 1.0f); });
        }
    }

    ui::subheading("Stages");
    const ui::RailEntry stages[] = {
        {.label = "Spawn"},
        {.label = "Burst", .icon = ui::icon::add, .item = 0, .note = "every 2 s", .selected = stage_ == 0},
        {.label = "Update"},
        {.label = "Drift", .icon = ui::icon::follow, .item = 1, .selected = stage_ == 1},
        {.label = "Render"},
    };
    if (const auto item = ui::rail("##stages", stages, theme::Hue::orange)) stage_ = *item;

    ui::subheading("Layers");
    std::array<ui::Bit, LAYERS.size()> bits{};
    for (std::size_t i = 0; i < bits.size(); ++i) {
        const bool named = *LAYERS[i] != '\0';
        const bool set = (layers_ >> i & 1u) != 0;
        bits[i] = {.state = !named ? ui::BitState::unavailable
                            : i == 6 ? ui::BitState::inherited
                            : set    ? ui::BitState::on
                                     : ui::BitState::off,
                   .tooltip = named ? LAYERS[i] : nullptr};
    }
    if (const auto clicked = ui::bit_strip("##layers", bits)) layers_ ^= 1u << *clicked;
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (bits[i].state == ui::BitState::on) ui::bit_line(i, LAYERS[i]);
        if (bits[i].state == ui::BitState::inherited) ui::bit_line(i, LAYERS[i], "the ember prefab");
    }
}

} // namespace ftk::example
