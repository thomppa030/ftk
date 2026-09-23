#include "ui/inspectors/gameplay_inspectors.hpp"

#include "audio/audio_component.hpp"
#include "core/input_component.hpp"
#include "renderer/components/arrow_component.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "scene/component_meta.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/game_ui/ui_widget_component.hpp"
#include "ui/inspectors/asset_rows.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/color_field.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/list_editor.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/kit/text_field.hpp"

#include <imgui.h>

#include <string>
#include <vector>

namespace fjell {

namespace {

constexpr ui::Choice<UiWidgetComponent::RenderMode> RENDER_MODES[] = {
    {UiWidgetComponent::RenderMode::screen_space, "Screen space"},
    {UiWidgetComponent::RenderMode::world_space, "World space"},
};

constexpr ui::Choice<UiWidgetComponent::Scaling> SCALINGS[] = {
    {UiWidgetComponent::Scaling::design_resolution, "Design resolution"},
    {UiWidgetComponent::Scaling::native, "Native pixels"},
    {UiWidgetComponent::Scaling::custom, "Custom height"},
};

// Style classes are typed the way a class="..." attribute reads: separated
// by spaces.
std::string join_classes(const std::vector<std::string>& classes) {
    std::string joined;
    for (const auto& cls : classes) {
        if (!joined.empty()) joined += ' ';
        joined += cls;
    }
    return joined;
}

std::vector<std::string> split_classes(const std::string& text) {
    std::vector<std::string> classes;
    std::string token;
    for (char c : text) {
        if (c == ' ') {
            if (!token.empty()) classes.push_back(std::move(token));
            token.clear();
        } else {
            token += c;
        }
    }
    if (!token.empty()) classes.push_back(std::move(token));
    return classes;
}

} // namespace

ui::Edit draw_audio_source(Node3D& /*node*/, void* component, const InspectorContext& ctx) {
    auto& au = *static_cast<AudioSourceComponent*>(component);
    static const std::string audio_exts[] = {".wav", ".ogg", ".mp3", ".fjaudio"};
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##audio_source")) {
        ui::row("Clip", [&] {
            if (asset_slot({.label = "Clip", .id = "##clip", .extensions = audio_exts, .ctx = &ctx}, au.clip_path)) {
                au.sound_loaded = false;
                edit = {true, true};
            }
        });
        ui::row("Volume", [&] {
            edit |= ui::drag("##volume", au.volume, {.speed = 0.01f, .min = 0.0f, .max = 2.0f, .unit = ui::Unit::Times});
        });
        ui::row("Pitch", [&] {
            edit |= ui::drag("##pitch", au.pitch, {.speed = 0.01f, .min = 0.1f, .max = 4.0f, .unit = ui::Unit::Times});
        });
        ui::row("Loop", [&] { edit |= ui::checkbox("##loop", au.loop); });
        ui::row("Play on start", [&] { edit |= ui::checkbox("##play_on_start", au.play_on_start); });
        ui::row("Spatial", "Heard from where the node is: quieter with distance and placed left or right.",
                [&] { edit |= ui::checkbox("##spatial", au.spatial); });
        if (au.spatial) {
            ui::row("Min distance", "Full volume this close.", [&] {
                edit |= ui::drag("##min_distance", au.min_distance,
                                 {.speed = 0.1f, .min = 0.1f, .max = 100.0f, .unit = ui::Unit::Metres});
            });
            ui::row("Max distance", "Past this the sound gets no quieter.", [&] {
                edit |= ui::drag("##max_distance", au.max_distance,
                                 {.speed = 1.0f, .min = 1.0f, .max = 1000.0f, .unit = ui::Unit::Metres});
            });
        }
    }
    return edit;
}

ui::Edit draw_particle_emitter(Node3D& /*node*/, void* component, const InspectorContext& ctx) {
    auto& em = *static_cast<EmitterComponent*>(component);
    static const std::string vfx_exts[] = {".fjvfx"};
    ui::Edit edit;
    if (!em.vfx_path.empty() && !em.effect) {
        ui::status(ui::Severity::Error, "The effect didn't load; the console says why");
    }
    if (auto t = ui::PropertyTable("##particle_emitter")) {
        ui::row("Effect", [&] {
            if (asset_slot({.label = "Effect", .id = "##effect", .extensions = vfx_exts, .ctx = &ctx}, em.vfx_path)) {
                if (ctx.resource_manager != nullptr) {
                    load_emitter_effect(em, *ctx.resource_manager, project_of(ctx));
                }
                edit = {true, true};
            }
        });
        ui::row("Looping", "Fires its bursts again once every particle is gone.",
                [&] { edit |= ui::checkbox("##looping", em.looping); });
        ui::row("", [&] {
            ImGui::BeginDisabled(!em.effect);
            // Previews the bursts now; nothing is saved.
            if (ui::action(ui::icon::play, "Fire")) em.fire = true;
            ImGui::EndDisabled();
            ImGui::SetItemTooltip(em.effect ? "Fire the effect's bursts now" : "Needs an effect first");
        });
    }
    return edit;
}

ui::Edit draw_ui_widget(Node3D& /*node*/, void* component, const InspectorContext& ctx) {
    using RenderMode = UiWidgetComponent::RenderMode;
    using Scaling = UiWidgetComponent::Scaling;
    auto& wc = *static_cast<UiWidgetComponent*>(component);
    static const std::string widget_exts[] = {".fjwidget"};
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##ui_widget")) {
        ui::row("Widget", [&] {
            if (asset_slot({.label = "Widget", .id = "##widget", .extensions = widget_exts, .ctx = &ctx},
                           wc.widget_path)) {
                edit = {true, true};
            }
        });
        ui::row("Style classes", "Classes added to the widget's root for this placement, separated by spaces.",
                [&] {
                    std::string text = join_classes(wc.style_classes);
                    if (ui::text_field("##style_classes", text).committed) {
                        wc.style_classes = split_classes(text);
                        edit = {true, true};
                    }
                });
        ui::row("Render mode", [&] { edit |= ui::choice("##render_mode", wc.render_mode, RENDER_MODES); });
        if (wc.render_mode == RenderMode::screen_space) {
            ui::row("Scaling",
                    "Design resolution: the project's design height fills the viewport.\n"
                    "Native pixels: one layout pixel per screen pixel.\n"
                    "Custom height: this many layout pixels fill the viewport.",
                    [&] { edit |= ui::choice("##scaling", wc.scaling, SCALINGS); });
            if (wc.scaling == Scaling::custom) {
                ui::row("Reference height", [&] {
                    edit |= ui::drag("##reference_height", wc.custom_reference_height,
                                     {.speed = 1.0f, .min = 240.0f, .max = 4320.0f,
                                      .unit = ui::Unit::Pixels, .format = "%.0f"});
                });
            }
        } else {
            ui::row("Pixels per unit", "How many layout pixels make one metre.", [&] {
                edit |= ui::drag("##pixels_per_unit", wc.pixels_per_unit,
                                 {.speed = 1.0f, .min = 1.0f, .max = 1000.0f, .format = "%.0f"});
            });
            ui::row("Billboard", "Always turned to face the camera.",
                    [&] { edit |= ui::checkbox("##billboard", wc.billboard); });
            ui::row("Virtual viewport", "The screen size the widget lays out in.", [&] {
                edit |= ui::vec2("##virtual_viewport", wc.virtual_viewport,
                                 {.speed = 1.0f, .min = 1.0f, .max = 2048.0f,
                                  .unit = ui::Unit::Pixels, .format = "%.0f"});
            });
            ui::row("Offset", [&] {
                edit |= ui::vec3("##offset", wc.offset, {.speed = 0.05f, .unit = ui::Unit::Metres});
            });
        }
    }
    return edit;
}

ui::Edit draw_input(Node3D& /*node*/, void* component, const InspectorContext& ctx) {
    auto& ic = *static_cast<InputComponent*>(component);
    static const std::string context_exts[] = {".fjinput"};
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##input")) {
        ui::row("Actions folder", "The folder of .fjaction files, from the project root.", [&] {
            edit |= ui::text_field("##actions_dir", ic.actions_dir);
        });
        if (ic.actions_dir.empty()) ui::hint("Empty reads assets/input/actions");
    }

    // Pushed in this order when play starts, so the last is on top and
    // answers input first.
    ui::subheading("Contexts");
    edit |= ui::list_editor("##contexts", ic.context_paths, "Add context",
                            "No contexts. Add the input contexts this node turns on while playing",
                            [&](std::string& path, std::size_t) {
                                ImGui::SetNextItemWidth(-FLT_MIN);
                                const bool changed = asset_slot({.label = "Context", .id = "##context",
                                                                 .extensions = context_exts, .ctx = &ctx},
                                                                path);
                                return ui::Edit{changed, changed};
                            });
    if (ic.context_paths.size() > 1) ui::hint("The last one answers input first");
    return edit;
}

ui::Edit draw_arrow(Node3D& /*node*/, void* component, const InspectorContext& /*ctx*/) {
    auto& arrow = *static_cast<ArrowComponent*>(component);
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##arrow")) {
        ui::row("Length", [&] {
            edit |= ui::drag("##length", arrow.length,
                             {.speed = 0.1f, .min = 0.1f, .max = 50.0f, .unit = ui::Unit::Metres});
        });
        ui::row("Colour", [&] { edit |= ui::color("##colour", arrow.color, ui::ColorSpace::Srgb); });
    }
    return edit;
}

} // namespace fjell
