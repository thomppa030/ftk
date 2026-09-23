#include "ui/kit/gallery.hpp"

#include "ui/inspector_widgets.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/color_field.hpp"
#include "ui/kit/component_block.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/kit/text_field.hpp"
#include "ui/panel_widget.hpp"
#include "ui/theme.hpp"

#include <imgui.h>

#include <cstdio>
#include <string>

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
        const std::pair<const char*, theme::Category> categories[] = {
            {"Rendering", theme::Category::Rendering}, {"Light", theme::Category::Light},
            {"Camera", theme::Category::Camera}, {"Environment", theme::Category::Environment},
            {"Physics", theme::Category::Physics}, {"Animation", theme::Category::Animation},
            {"Audio", theme::Category::Audio}, {"VFX", theme::Category::Vfx},
            {"UI", theme::Category::Ui}, {"Logic", theme::Category::Logic},
            {"Structure", theme::Category::Structure},
        };
        for (const auto& [name, c] : categories) swatch(name, theme::category(c));

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
            }
            if (remove) removed_ = name;
            ImGui::PopID();
        }
        if (removed_ != nullptr) {
            char text[64];
            std::snprintf(text, sizeof(text), "Remove clicked on %s", removed_);
            hint(text);
        }
    }
}

void KitGallery::feedback() {
    if (section_foldable("Feedback", icon::more)) {
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
        feedback();
    }
}

} // namespace fjell::ui
