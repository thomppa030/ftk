#include "ui/kit/gallery.hpp"

#include "ui/asset_ref_widget.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/color_field.hpp"
#include "ui/kit/component_block.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/inset_group.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/list_editor.hpp"
#include "ui/kit/menu.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/search.hpp"
#include "ui/kit/section.hpp"
#include "ui/kit/text_field.hpp"
#include "ui/kit/tree.hpp"
#include "ui/panel_widget.hpp"
#include "ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace fjell::ui {

namespace {

// One colour token: a swatch, its name and its value.
void swatch(const char* name, const ImVec4& colour) {
    const float side = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + side * 1.5f, p.y + side},
                      ImGui::ColorConvertFloat4ToU32(colour), 3.0f);
    dl->AddRect(p, {p.x + side * 1.5f, p.y + side},
                ImGui::ColorConvertFloat4ToU32(theme::border()), 3.0f);
    ImGui::Dummy({side * 1.5f, side});
    ImGui::SameLine(0.0f, theme::GAP_M);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    ImGui::SameLine(0.0f, theme::GAP_M);
    char value[24];
    std::snprintf(value, sizeof(value), "#%02X%02X%02X",
                  static_cast<int>(colour.x * 255.0f + 0.5f),
                  static_cast<int>(colour.y * 255.0f + 0.5f),
                  static_cast<int>(colour.z * 255.0f + 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::TextUnformatted(value);
    if (colour.w < 1.0f) {
        ImGui::SameLine();
        ImGui::Text("@ %d %%", static_cast<int>(colour.w * 100.0f + 0.5f));
    }
    ImGui::PopStyleColor();
}

void tokens() {
    if (section_foldable("Tokens", icon::more)) {
        subheading("Surfaces");
        swatch("surface_sunken", theme::surface_sunken());
        swatch("surface_base", theme::surface_base());
        swatch("surface_raised", theme::surface_raised());
        swatch("surface_hover", theme::surface_hover());
        swatch("surface_active", theme::surface_active());
        swatch("surface_highest", theme::surface_highest());
        swatch("border", theme::border());

        subheading("Text");
        swatch("text", theme::text());
        swatch("text_secondary", theme::text_secondary());
        swatch("text_disabled", theme::text_disabled());

        subheading("Accent and state");
        swatch("accent", theme::accent());
        swatch("accent_bright", theme::accent_bright());
        swatch("accent_hov", theme::accent_hov());
        swatch("selection", theme::selection());
        swatch("selection_secondary", theme::selection_secondary());
        swatch("toggle_on", theme::toggle_on());

        subheading("Status");
        swatch("success", theme::success());
        swatch("warning", theme::warning());
        swatch("error", theme::error());
        swatch("danger", theme::danger());
        swatch("danger_hov", theme::danger_hov());

        subheading("Categories");
        for (int c = 0; c <= static_cast<int>(theme::Category::Structure); ++c) {
            const auto category = static_cast<theme::Category>(c);
            swatch(theme::category_name(category), theme::category(category));
        }

        subheading("Axes and canvas");
        swatch("axis_x", theme::axis_x());
        swatch("axis_y", theme::axis_y());
        swatch("axis_z", theme::axis_z());
        swatch("grid_minor", theme::grid_minor());
        swatch("grid_major", theme::grid_major());
        swatch("grid_zero", theme::grid_zero());
    }
}

void headings() {
    if (section_foldable("Headings", icon::more)) {
        section("Section", icon::search);
        ImGui::TextUnformatted("A part of the panel, with a rule and its own icon.");
        subheading("Sub-heading");
        ImGui::TextUnformatted("A named group inside a section or component.");
        if (subheading_foldable("Foldable sub-heading")) {
            ImGui::TextUnformatted("Folds with the chevron on the left.");
        }
    }
}

} // namespace

void KitGallery::buttons() {
    if (section_foldable("Buttons", icon::more)) {
        const std::pair<const char*, ButtonKind> kinds[] = {
            {"Secondary", ButtonKind::Secondary}, {"Primary", ButtonKind::Primary},
            {"Ghost", ButtonKind::Ghost}, {"Danger", ButtonKind::Danger},
            {"Ghost danger", ButtonKind::GhostDanger},
        };
        for (bool disabled : {false, true}) {
            subheading(disabled ? "Disabled" : "Kinds");
            ImGui::BeginDisabled(disabled);
            bool first = true;
            for (const auto& [name, kind] : kinds) {
                if (!first) ImGui::SameLine();
                first = false;
                button(name, kind);
            }
            ImGui::EndDisabled();
        }

        subheading("Icon buttons");
        icon_button("add", icon::add, "Add");
        ImGui::SameLine();
        icon_button("browse", icon::browse, "Browse");
        ImGui::SameLine();
        icon_button("use_selected", icon::use_selected, "Use selected");
        ImGui::SameLine();
        icon_button("clear", icon::clear, "Clear");
        ImGui::SameLine();
        icon_button("remove", icon::remove, "Remove (undoable)", ButtonKind::GhostDanger);

        subheading("Toggles");
        if (toggle_button("snap", ICON_LC_MAGNET, snap_, "Snap to grid")) snap_ = !snap_;
        ImGui::SameLine();
        if (toggle_button("grid", ICON_LC_GRID_3X3, grid_, "Show grid")) grid_ = !grid_;
        ImGui::SameLine();
        if (toggle("Paint", paint_, "A text toggle")) paint_ = !paint_;
    }
}

void KitGallery::rows() {
    if (section_foldable("Rows", icon::more)) {
        char width[64];
        std::snprintf(width, sizeof(width), "Label column at this width: %.0f px",
                      theme::label_column(ImGui::GetContentRegionAvail().x));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
        ImGui::TextUnformatted(width);
        ImGui::PopStyleColor();

        if (auto t = PropertyTable("##gallery_rows")) {
            row("Name", [&] { text_field("##name", name_); });
            row("Intensity", "Brightness of the light, as a multiple of the colour.",
                [&] { drag("##intensity", intensity_, {.speed = 0.05f, .min = 0.0f, .max = 100.0f}); });
            hint("Above 1 blooms");
            row("Cast shadows", [&] { checkbox("##shadows", shadows_); });
        }
    }
}

void KitGallery::fields() {
    if (section_foldable("Fields", icon::more)) {
        Edit edit;
        subheading("Units");
        if (auto t = PropertyTable("##gallery_units")) {
            row("Mass", [&] { edit |= drag("##mass", mass_, {.speed = 0.1f, .min = 0.01f, .max = 1000.0f, .unit = Unit::Kilograms, .format = "%.1f"}); });
            row("Field of view", [&] { edit |= drag("##fov", fov_, {.speed = 0.5f, .min = 1.0f, .max = 179.0f, .unit = Unit::Degrees, .format = "%.1f"}); });
            row("Rayleigh height", [&] { edit |= drag("##rayleigh", rayleigh_, {.speed = 10.0f, .unit = Unit::Metres, .format = "%.0f"}); });
            row("Wind area", [&] { edit |= drag("##wind", wind_area_, {.speed = 0.05f, .min = 0.0f, .max = 100.0f, .unit = Unit::SquareMetres}); });
            row("Day length", [&] { edit |= drag("##day", day_length_, {.speed = 0.1f, .min = 0.1f, .max = 1440.0f, .unit = Unit::Minutes, .format = "%.1f"}); });
            row("Slope limit", [&] { edit |= slider("##slope", slope_, 0.0f, 90.0f, Unit::Degrees, "%.1f"); });
            row("Count", [&] { edit |= drag_int("##count", count_, 0.5f, 0, 1024); });
        }
        subheading("Vectors");
        if (auto t = PropertyTable("##gallery_vectors")) {
            row("Position", [&] { edit |= vec3("##position", position_); });
            row("Rotation", [&] { edit |= vec3("##rotation", rotation_, {.speed = 1.0f, .unit = Unit::Degrees, .format = "%.1f"}); });
            row("Scale", [&] { edit |= vec3("##scale", scale_, {.speed = 0.05f, .min = 0.01f, .max = 100.0f}); });
            row("Tiling", [&] { edit |= vec2("##tiling", tiling_, {.speed = 0.05f, .unit = Unit::Times}); });
        }
        subheading("Asset slots");
        if (auto t = PropertyTable("##gallery_slots")) {
            static const std::string shader_exts[] = {".fjsl"};
            static const std::string material_exts[] = {".fjmat"};
            row("Shader", [&] {
                edit.changed |= asset_slot({.label = "Shader", .id = "##shader", .extensions = shader_exts},
                                           slot_shader_);
            });
            row("Material", [&] {
                edit.changed |= asset_slot({.label = "Material", .id = "##material", .extensions = material_exts},
                                           slot_empty_);
            });
            row("Surface", [&] {
                edit.changed |= asset_slot({.label = "Surface", .id = "##missing", .extensions = material_exts},
                                           slot_missing_);
            });
        }
        subheading("Toggles");
        if (auto t = PropertyTable("##gallery_toggles")) {
            row("Cast shadows", [&] { edit |= checkbox("##shadows", shadows_); });
        }
        edit |= choices();
        if (edit.committed) ++commits_;
        char count[48];
        std::snprintf(count, sizeof(count), "Edits committed: %d", commits_);
        hint(count);
    }
}

Edit KitGallery::choices() {
    static constexpr Choice<Shape> SHAPES[] = {
        {Shape::Box, "Box"}, {Shape::Sphere, "Sphere"},
        {Shape::Capsule, "Capsule"}, {Shape::Mesh, "Mesh"},
    };
    static constexpr Choice<Blend> BLENDS[] = {
        {Blend::Opaque, "Opaque"}, {Blend::Alpha, "Alpha"},
        {Blend::Additive, "Additive"}, {Blend::Multiply, "Multiply"},
    };
    static constexpr Choice<Body> BODIES[] = {
        {Body::Static, "Static"}, {Body::Dynamic, "Dynamic"}, {Body::Kinematic, "Kinematic"},
    };

    Edit edit;
    subheading("Choices");
    if (auto t = PropertyTable("##gallery_choices")) {
        row("Shape", [&] { edit |= choice("##shape", shape_, SHAPES); });
        row("Blend mode", [&] { edit |= choice("##blend", blend_, BLENDS); });
        row("Body type", [&] { edit |= choice("##body", body_, BODIES); });
    }
    subheading("Colours");
    if (auto t = PropertyTable("##gallery_colours")) {
        row("Light colour", [&] { edit |= color("##light", light_colour_, ColorSpace::Linear); });
        row("Intensity", [&] { edit |= drag("##intensity", intensity_, {.speed = 0.05f, .min = 0.0f, .max = 100.0f}); });
        row("Tint", [&] { edit |= color("##tint", tint_, ColorSpace::Srgb); });
    }
    return edit;
}

void KitGallery::blocks() {
    if (section_foldable("Component blocks", icon::more)) {
        const std::pair<const char*, theme::Category> samples[] = {
            {"Mesh", theme::Category::Rendering},
            {"Collider", theme::Category::Physics},
            {"Audio Source", theme::Category::Audio},
        };
        for (const auto& [name, category] : samples) {
            ImGui::PushID(name);
            bool remove = false;
            if (component_block(name, category, remove)) {
                if (auto t = PropertyTable("##body")) {
                    row("Cast shadows", [&] { checkbox("##shadows", shadows_); });
                }
                // A mesh's material values, set apart in their box.
                if (category == theme::Category::Rendering) {
                    if (auto box = InsetGroup("##material", "Material", icon::material,
                                              theme::category(theme::Category::Rendering))) {
                        if (auto t = PropertyTable("##material_rows")) {
                            row("Roughness", [&] { slider("##roughness", slope_, 0.0f, 90.0f); });
                            row("Tint", [&] { color("##tint", tint_, ColorSpace::Srgb); });
                        }
                    }
                }
                if (auto t = PropertyTable("##body_more")) {
                    // An action sits in the value column beside what it acts
                    // on, one click away.
                    if (category == theme::Category::Physics) {
                        row("Size", [&] { vec3("##size", scale_); });
                        row("", [&] {
                            if (action(icon::use_selected, "Fit to mesh")) removed_ = "Fit to mesh";
                            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                            if (action(icon::remove, "Clear points", ButtonKind::GhostDanger)) {
                                removed_ = "Clear points";
                            }
                        });
                    }
                }
            }
            if (remove) removed_ = name;
            ImGui::PopID();
        }
        if (removed_ != nullptr) {
            char text[64];
            std::snprintf(text, sizeof(text), "Last clicked: %s", removed_);
            hint(text);
        }
    }
}

void KitGallery::lists() {
    if (section_foldable("Lists", icon::more)) {
        Edit edit;
        subheading("Points");
        edit |= list_editor("##points", points_, "Add point", "No points. The collider's corners are used",
                            [](glm::vec3& p, std::size_t) { return vec3("##p", p); });
        subheading("Names");
        edit |= list_editor("##names", names_, "Add name", "No names yet",
                            [](std::string& name, std::size_t) { return text_field("##name", name); },
                            [] { return std::string("new"); });
        subheading("Events");
        edit |= list_editor("##events", events_, "Add event",
                            "No events. Add one to call a script at a frame",
                            [](float& t, std::size_t) { return drag("##t", t, {.unit = Unit::Seconds}); });
        if (edit.committed) ++commits_;

        subheading("Tree");
        search_field("##gallery_search", query_);
        tree_sample();
    }
}

void KitGallery::graphs() {
    if (!section_foldable("Node graphs", icon::node_graph)) return;
    // What both samples do with what the user did: move a node, add a link,
    // remove the selected link or a pin's. Their nodes stay.
    const auto apply = [this](NodeGraph& graph, const NodeGraphEvents& events, std::vector<glm::vec2>& places,
                              std::vector<NodeLink>& links) {
        if (events.moved) places[static_cast<std::size_t>(events.moved->node - 1)] = events.moved->position;
        if (events.linked) {
            links.push_back({.id = next_link_++, .from = events.linked->from, .to = events.linked->to});
            graph.select_link(links.back().id);
        }
        if (events.remove && graph.selected_link() != 0) {
            std::erase_if(links, [&](const NodeLink& l) { return l.id == graph.selected_link(); });
        }
        for (const uint64_t id : events.unlinked) {
            std::erase_if(links, [&](const NodeLink& l) { return l.id == id; });
        }
    };
    const auto menu = [](const NodeGraphMenu& target) {
        if (target.link == 0) {
            (void)menu_item({.label = "Remove link", .enabled = false, .disabled_reason = "Right-click a link"});
            return false;
        }
        return menu_item({.icon = icon::remove, .label = "Remove link", .shortcut = "Del", .destructive = true});
    };

    subheading("By pins");
    {
        const ImVec4 time_hue = theme::category(theme::Category::Logic);
        const ImVec4 float_colour = theme::category(theme::Category::Rendering);
        NodeGraphDesc desc;
        NodeRow time;
        time.output = NodePin{.id = 11, .label = "Seconds", .colour = float_colour};
        desc.nodes.push_back({.id = 1, .position = pin_places_[0], .width = 150.0f, .title = "Time",
                              .hue = time_hue, .rows = {time}});
        NodeRow a;
        a.input = NodePin{.id = 21, .label = "A", .colour = float_colour};
        a.output = NodePin{.id = 23, .label = "Result", .colour = float_colour};
        NodeRow b;
        b.input = NodePin{.id = 22, .label = "B", .colour = float_colour};
        b.value = NodeValue::number(multiply_by_, {.speed = 0.01f});
        desc.nodes.push_back({.id = 2, .position = pin_places_[1], .width = 150.0f, .title = "Multiply",
                              .hue = time_hue, .rows = {a, b}});
        NodeRow rate;
        rate.input = NodePin{.id = 31, .label = "Rate", .colour = float_colour};
        const bool rate_linked = std::any_of(pin_links_.begin(), pin_links_.end(),
                                             [](const NodeLink& l) { return l.to.pin == 31; });
        if (!rate_linked) rate.value = NodeValue::number(spawn_rate_, {.speed = 1.0f, .lo = 0.0f, .hi = 1000.0f});
        desc.nodes.push_back({.id = 3, .position = pin_places_[2], .title = "Spawn rate",
                              .hue = theme::category(theme::Category::Environment), .rows = {rate}});
        desc.links = pin_links_;
        desc.can_link = [this](const NodeEnd&, const NodeEnd& to) {
            // One link into an input.
            return std::none_of(pin_links_.begin(), pin_links_.end(), [&](const NodeLink& l) { return l.to == to; });
        };
        if (ImGui::BeginChild("##pin_graph", {0.0f, 240.0f}, ImGuiChildFlags_Borders)) {
            apply(pin_graph_, pin_graph_.draw(desc, menu), pin_places_, pin_links_);
        }
        ImGui::EndChild();
    }

    subheading("By edge");
    {
        constexpr const char* NAMES[] = {"Idle", "Walk", "Run"};
        constexpr const char* CLIPS[] = {"idle", "walk_cycle", ""};
        NodeGraphDesc desc;
        for (std::size_t i = 0; i < std::size(NAMES); ++i) {
            NodeRow what;
            what.text = CLIPS[i][0] != '\0' ? CLIPS[i] : "Nothing to play";
            what.text_dimmed = CLIPS[i][0] == '\0';
            desc.nodes.push_back({.id = i + 1,
                                  .position = state_places_[i],
                                  .anchor = NodeAnchor::Centre,
                                  .width = 160.0f,
                                  .title = NAMES[i],
                                  .hue = theme::category(theme::Category::Animation),
                                  .connect = NodeConnect::Edge,
                                  .rows = {what}});
        }
        desc.links = state_links_;
        desc.start = NodeStart{.node = 1, .label = "Entry"};
        if (ImGui::BeginChild("##edge_graph", {0.0f, 240.0f}, ImGuiChildFlags_Borders)) {
            apply(edge_graph_, edge_graph_.draw(desc, menu), state_places_, state_links_);
        }
        ImGui::EndChild();
    }
}

void KitGallery::tree_sample() {
    using C = theme::Category;
    struct Row {
        int depth;
        const char* icon;
        std::optional<C> category;
        std::vector<C> dots;
    };
    // Sheet 6's harbour, depth first: a row's descendants follow it.
    static const Row rows[] = {
        {0, icon::folder, C::Structure, {}},
        {1, icon::directional_light, C::Light, {}},
        {1, icon::camera, C::Camera, {}},
        {1, icon::folder, C::Structure, {}},
        {2, icon::mesh, std::nullopt, {C::Rendering, C::Physics, C::Audio, C::Logic}},
        {2, icon::mesh, std::nullopt, {C::Rendering, C::Physics}},
        {2, icon::mesh, std::nullopt, {C::Rendering, C::Animation}},
        {3, icon::mesh, std::nullopt, {C::Rendering, C::Physics}},
        {1, icon::volumetric_fog, C::Environment, {}},
    };
    constexpr std::size_t count = std::size(rows);

    // A row stays, dimmed, while something under it matches.
    auto shown = [&](std::size_t i, bool& dimmed) {
        const bool own = matches(tree_names_[i], query_);
        bool below = false;
        for (std::size_t j = i + 1; j < count && rows[j].depth > rows[i].depth; ++j) {
            below = below || matches(tree_names_[j], query_);
        }
        dimmed = !own;
        return own || below;
    };

    if (tree_selected_ < count && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_F2)) {
        tree_rename_.start(static_cast<std::uint32_t>(tree_selected_), tree_names_[tree_selected_]);
    }
    if (auto tree = Tree("##gallery_tree")) {
        int folded_below = -1;
        for (std::size_t i = 0; i < count; ++i) {
            const Row& row = rows[i];
            if (folded_below >= 0 && row.depth > folded_below) continue;
            folded_below = -1;
            bool dimmed = false;
            if (!shown(i, dimmed)) continue;
            const bool has_children = i + 1 < count && rows[i + 1].depth > row.depth;
            const std::string id = std::to_string(i);
            const auto result = tree_row({
                .id = id.c_str(), .name = tree_names_[i], .depth = row.depth, .has_children = has_children,
                .icon = row.icon, .category = row.category, .dots = row.dots,
                .selection = i == tree_selected_ ? RowSelection::Primary
                           : i == 5            ? RowSelection::Secondary
                                               : RowSelection::None,
                .query = query_, .dimmed = dimmed, .force_open = !query_.empty(),
                .rename = &tree_rename_, .key = static_cast<std::uint32_t>(i)});
            if (result.clicked) tree_selected_ = i;
            if (result.renamed) tree_names_[i] = *result.renamed;
            if (has_children && !result.open) folded_below = row.depth;
        }
    }
}

void KitGallery::feedback() {
    if (section_foldable("Feedback", icon::more)) {
        subheading("Menu");
        if (button("Open menu")) ImGui::OpenPopup("##gallery_menu");
        if (ImGui::BeginPopup("##gallery_menu")) {
            menu_item({.icon = icon::rename, .label = "Rename", .shortcut = "F2"});
            menu_item({.icon = icon::duplicate, .label = "Duplicate", .shortcut = "Ctrl D"});
            menu_item({.label = "Copy path"});
            menu_item({.icon = icon::add, .label = "Create material", .enabled = false,
                       .disabled_reason = "Needs a terrain asset first"});
            ImGui::Separator();
            menu_item({.icon = icon::remove, .label = "Delete", .shortcut = "Del", .destructive = true});
            ImGui::EndPopup();
        }
        subheading("Empty state");
        if (ImGui::BeginChild("##empty", {0.0f, 180.0f}, ImGuiChildFlags_Borders)) {
            if (empty_state(icon::nothing_selected, "Nothing selected",
                            "Pick an object in the viewport or the hierarchy to edit it here",
                            "Select all")) {
                ++empty_clicks_;
            }
        }
        ImGui::EndChild();
        if (empty_clicks_ > 0) {
            char text[48];
            std::snprintf(text, sizeof(text), "Action clicked %d times", empty_clicks_);
            hint(text);
        }
    }
}

void KitGallery::draw(bool* open) {
    ImGui::SetNextWindowSize({420.0f, 640.0f}, ImGuiCond_FirstUseEver);
    if (auto p = Panel(ICON_LC_PALETTE, "Kit Gallery", open)) {
        tokens();
        headings();
        buttons();
        rows();
        fields();
        blocks();
        lists();
        graphs();
        feedback();
    }
}

} // namespace fjell::ui
