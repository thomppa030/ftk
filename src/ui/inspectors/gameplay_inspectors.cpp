#include "ui/inspectors/gameplay_inspectors.hpp"

#include "audio/audio_component.hpp"
#include "core/input_component.hpp"
#include "renderer/components/arrow_component.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "scene/component_meta.hpp"
#include "scene/node.hpp"
#include "core/base_path.hpp"
#include "core/project_manager.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/game_ui/ui_widget_component.hpp"
#include "ui/game_ui/widget_asset.hpp"
#include "ui/inspectors/asset_rows.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/color_field.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/list_editor.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ui/node_slot.hpp"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

namespace {

constexpr ftk::ui::Choice<UiWidgetComponent::RenderMode> RENDER_MODES[] = {
    {UiWidgetComponent::RenderMode::screen_space, "Screen space"},
    {UiWidgetComponent::RenderMode::world_space, "World space"},
};

constexpr ftk::ui::Choice<UiWidgetComponent::Scaling> SCALINGS[] = {
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

// The <model>s a widget file declares, read again only when the file has
// changed and asked at most once a second: an inspector draws every frame.
const std::vector<ModelDecl>& declared_models(const std::string& file) {
    struct Read {
        std::filesystem::file_time_type written{};
        double checked_at{-1.0};
        std::vector<ModelDecl> models;
    };
    static std::unordered_map<std::string, Read> read;
    auto& entry = read[file];
    const double now = ImGui::GetTime();
    if (entry.checked_at >= 0.0 && now - entry.checked_at < 1.0) return entry.models;
    entry.checked_at = now;
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(file, ec);
    if (ec) {
        entry.models.clear();
        return entry.models;
    }
    if (written == entry.written) return entry.models;
    entry.written = written;
    entry.models = load_widget_models(file);
    return entry.models;
}

void set_model_node(UiWidgetComponent& wc, const std::string& model, uint32_t node) {
    std::erase_if(wc.model_nodes, [&](const UiWidgetComponent::ModelNode& picked) { return picked.model == model; });
    if (node != 0) wc.model_nodes.push_back({model, node});
}

// The widget's own <model>s, each with the node this placement reads it from:
// the markup's `from` rule until a node is picked. A pick for a Model the
// widget no longer declares stays until it is cleared, so a renamed Model
// loses nothing without being told.
ftk::ui::Edit draw_widget_models(UiWidgetComponent& wc, const InspectorContext& ctx) {
    const Project* project = ctx.project_mgr != nullptr ? ctx.project_mgr->active() : nullptr;
    const std::string file = wc.widget_path.empty() ? std::string()
                                                    : base_path::resolve_asset(wc.widget_path, project);
    static const std::vector<ModelDecl> none;
    const std::vector<ModelDecl>& declared = file.empty() ? none : declared_models(file);
    if (declared.empty() && wc.model_nodes.empty()) return {};

    const std::string file_name = std::filesystem::path(file).filename().string();
    ftk::ui::Edit edit;
    ftk::ui::subheading("Models");
    if (auto t = ftk::ui::PropertyTable("##ui_widget_models")) {
        for (const auto& model : declared) {
            const std::string help = "Reads " + model.component + " (" + file_name + ":" +
                                     std::to_string(model.source_line) + ").";
            ftk::ui::row(model.name.c_str(), help.c_str(), [&] {
                const bool local = model.from == ModelSource::local;
                uint32_t node = wc.model_node(model.name);
                const std::string id = "##model_" + model.name;
                const NodeSlotConfig slot{
                    .id = id.c_str(),
                    .scene = ctx.scene,
                    .refusal = [&](const Node& candidate) -> std::string {
                        for (const auto& script : candidate.scripts) {
                            if (script.type_name == model.component) return {};
                        }
                        return candidate.name + " has no " + model.component + "; this Model reads " +
                               model.component + ".";
                    },
                    .empty_glyph = local ? ftk::ui::icon::local_player : ftk::ui::icon::this_node,
                    .empty_text = local ? "Local player" : "This node",
                };
                if (node_slot(slot, node)) {
                    set_model_node(wc, model.name, node);
                    edit = {true, true};
                }
            });
        }
        for (std::size_t i = 0; i < wc.model_nodes.size(); ++i) {
            const std::string name = wc.model_nodes[i].model;
            if (std::ranges::any_of(declared, [&](const ModelDecl& m) { return m.name == name; })) continue;
            ftk::ui::row(name.c_str(), [&] {
                const float start = ImGui::GetCursorPosX();
                const float clear_at =
                    start + std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight(), 0.0f);
                const std::string gone = "No longer in " + file_name;
                ftk::ui::readout(gone.c_str());
                ImGui::SameLine(clear_at);
                const std::string id = "##forget_" + name;
                if (ftk::ui::icon_button(id.c_str(), ftk::ui::icon::clear, "Clear", ftk::ui::ButtonKind::GhostDanger)) {
                    set_model_node(wc, name, 0);
                    edit = {true, true};
                }
            });
        }
    }
    return edit;
}

} // namespace

ftk::ui::Edit draw_audio_source(Node3D& /*node*/, void* component, const InspectorContext& ctx) {
    auto& au = *static_cast<AudioSourceComponent*>(component);
    static const std::string audio_exts[] = {".wav", ".ogg", ".mp3", ".fjaudio"};
    ftk::ui::Edit edit;
    if (auto t = ftk::ui::PropertyTable("##audio_source")) {
        ftk::ui::row("Clip", [&] {
            if (asset_slot({.label = "Clip", .id = "##clip", .extensions = audio_exts, .ctx = &ctx}, au.clip_path)) {
                au.sound_loaded = false;
                edit = {true, true};
            }
        });
        ftk::ui::row("Volume", [&] {
            edit |= ftk::ui::drag("##volume", au.volume, {.speed = 0.01f, .min = 0.0f, .max = 2.0f, .unit = ftk::ui::Unit::Times});
        });
        ftk::ui::row("Pitch", [&] {
            edit |= ftk::ui::drag("##pitch", au.pitch, {.speed = 0.01f, .min = 0.1f, .max = 4.0f, .unit = ftk::ui::Unit::Times});
        });
        ftk::ui::row("Loop", [&] { edit |= ftk::ui::checkbox("##loop", au.loop); });
        ftk::ui::row("Play on start", [&] { edit |= ftk::ui::checkbox("##play_on_start", au.play_on_start); });
        ftk::ui::row("Spatial", "Heard from where the node is: quieter with distance and placed left or right.",
                [&] { edit |= ftk::ui::checkbox("##spatial", au.spatial); });
        if (au.spatial) {
            ftk::ui::row("Min distance", "Full volume this close.", [&] {
                edit |= ftk::ui::drag("##min_distance", au.min_distance,
                                 {.speed = 0.1f, .min = 0.1f, .max = 100.0f, .unit = ftk::ui::Unit::Metres});
            });
            ftk::ui::row("Max distance", "Past this the sound gets no quieter.", [&] {
                edit |= ftk::ui::drag("##max_distance", au.max_distance,
                                 {.speed = 1.0f, .min = 1.0f, .max = 1000.0f, .unit = ftk::ui::Unit::Metres});
            });
        }
    }
    return edit;
}

ftk::ui::Edit draw_particle_emitter(Node3D& /*node*/, void* component, const InspectorContext& ctx) {
    auto& em = *static_cast<EmitterComponent*>(component);
    static const std::string vfx_exts[] = {".fjvfx"};
    ftk::ui::Edit edit;
    if (!em.vfx_path.empty() && !em.effect) {
        ftk::ui::status(ftk::ui::Severity::Error, "The effect didn't load; the console says why");
    }
    if (auto t = ftk::ui::PropertyTable("##particle_emitter")) {
        ftk::ui::row("Effect", [&] {
            if (asset_slot({.label = "Effect", .id = "##effect", .extensions = vfx_exts, .ctx = &ctx}, em.vfx_path)) {
                if (ctx.resource_manager != nullptr) {
                    load_emitter_effect(em, *ctx.resource_manager, project_of(ctx));
                }
                edit = {true, true};
            }
        });
        ftk::ui::row("Looping", "Fires its bursts again once every particle is gone.",
                [&] { edit |= ftk::ui::checkbox("##looping", em.looping); });
        ftk::ui::row("", [&] {
            ImGui::BeginDisabled(!em.effect);
            // Previews the bursts now; nothing is saved.
            if (ftk::ui::action(ftk::ui::icon::play, "Fire")) em.fire = true;
            ImGui::EndDisabled();
            ImGui::SetItemTooltip(em.effect ? "Fire the effect's bursts now" : "Needs an effect first");
        });
    }
    return edit;
}

ftk::ui::Edit draw_ui_widget(Node3D& /*node*/, void* component, const InspectorContext& ctx) {
    using Scaling = UiWidgetComponent::Scaling;
    auto& wc = *static_cast<UiWidgetComponent*>(component);
    static const std::string widget_exts[] = {".fjwidget"};
    ftk::ui::Edit edit;
    if (auto t = ftk::ui::PropertyTable("##ui_widget")) {
        ftk::ui::row("Widget", [&] {
            if (asset_slot({.label = "Widget", .id = "##widget", .extensions = widget_exts, .ctx = &ctx},
                           wc.widget_path)) {
                edit = {true, true};
            }
        });
        ftk::ui::row("Style classes", "Classes added to the widget's root for this placement, separated by spaces.",
                [&] {
                    std::string text = join_classes(wc.style_classes);
                    if (ftk::ui::text_field("##style_classes", text).committed) {
                        wc.style_classes = split_classes(text);
                        edit = {true, true};
                    }
                });
        ftk::ui::row("Render mode", [&] { edit |= ftk::ui::choice("##render_mode", wc.render_mode, RENDER_MODES); });
        if (wc.render_mode == UiWidgetComponent::RenderMode::screen_space) {
            ftk::ui::row("Scaling",
                    "Design resolution: the project's design height fills the viewport.\n"
                    "Native pixels: one layout pixel per screen pixel.\n"
                    "Custom height: this many layout pixels fill the viewport.",
                    [&] { edit |= ftk::ui::choice("##scaling", wc.scaling, SCALINGS); });
            if (wc.scaling == Scaling::custom) {
                ftk::ui::row("Reference height", [&] {
                    edit |= ftk::ui::drag("##reference_height", wc.custom_reference_height,
                                     {.speed = 1.0f, .min = 240.0f, .max = 4320.0f,
                                      .unit = ftk::ui::Unit::Pixels, .format = "%.0f"});
                });
            }
        } else {
            ftk::ui::row("Pixels per unit", "How many layout pixels make one metre.", [&] {
                edit |= ftk::ui::drag("##pixels_per_unit", wc.pixels_per_unit,
                                 {.speed = 1.0f, .min = 1.0f, .max = 1000.0f, .format = "%.0f"});
            });
            ftk::ui::row("Billboard", "Always turned to face the camera.",
                    [&] { edit |= ftk::ui::checkbox("##billboard", wc.billboard); });
            ftk::ui::row("Virtual viewport", "The screen size the widget lays out in.", [&] {
                edit |= ftk::ui::vec2("##virtual_viewport", wc.virtual_viewport,
                                 {.speed = 1.0f, .min = 1.0f, .max = 2048.0f,
                                  .unit = ftk::ui::Unit::Pixels, .format = "%.0f"});
            });
            ftk::ui::row("Offset", [&] {
                edit |= ftk::ui::vec3("##offset", wc.offset, {.speed = 0.05f, .unit = ftk::ui::Unit::Metres});
            });
        }
    }
    edit |= draw_widget_models(wc, ctx);
    return edit;
}

ftk::ui::Edit draw_input(Node3D& /*node*/, void* component, const InspectorContext& ctx) {
    auto& ic = *static_cast<InputComponent*>(component);
    static const std::string context_exts[] = {".fjinput"};
    ftk::ui::Edit edit;
    if (auto t = ftk::ui::PropertyTable("##input")) {
        ftk::ui::row("Actions folder", "The folder of .fjaction files, from the project root.", [&] {
            edit |= ftk::ui::text_field("##actions_dir", ic.actions_dir);
        });
        if (ic.actions_dir.empty()) ftk::ui::hint("Empty reads assets/input/actions");
    }

    // Pushed in this order when play starts, so the last is on top and
    // answers input first.
    ftk::ui::subheading("Contexts");
    edit |= ftk::ui::list_editor("##contexts", ic.context_paths, "Add context",
                            "No contexts. Add the input contexts this node turns on while playing",
                            [&](std::string& path, std::size_t) {
                                ImGui::SetNextItemWidth(-FLT_MIN);
                                const bool changed = asset_slot({.label = "Context", .id = "##context",
                                                                 .extensions = context_exts, .ctx = &ctx},
                                                                path);
                                return ftk::ui::Edit{changed, changed};
                            });
    if (ic.context_paths.size() > 1) ftk::ui::hint("The last one answers input first");
    return edit;
}

ftk::ui::Edit draw_arrow(Node3D& /*node*/, void* component, const InspectorContext& /*ctx*/) {
    auto& arrow = *static_cast<ArrowComponent*>(component);
    ftk::ui::Edit edit;
    if (auto t = ftk::ui::PropertyTable("##arrow")) {
        ftk::ui::row("Length", [&] {
            edit |= ftk::ui::drag("##length", arrow.length,
                             {.speed = 0.1f, .min = 0.1f, .max = 50.0f, .unit = ftk::ui::Unit::Metres});
        });
        ftk::ui::row("Colour", [&] { edit |= ftk::ui::color("##colour", arrow.color, ftk::ui::ColorSpace::Srgb); });
    }
    return edit;
}

} // namespace fjell
