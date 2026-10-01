#include "ui/water_editor_context.hpp"

#include "core/asset_type_registry.hpp"
#include "core/base_path.hpp"
#include "ftk/base/log.hpp"
#include "core/project.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/fjwater_io.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/material_instance.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/water_spectrum.hpp"
#include "renderer/resources/water_surface.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/asset_ref_widget.hpp"
#include "renderer/resources/asset_shader_type.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ui/icons.hpp"
#include "ftk/ui/kit/list_editor.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <variant>

namespace fjell {

namespace {

/// A wave train's direction is authored as an angle, because that is how a
/// sea is read — "this one runs across the others" — rather than as a pair
/// of components that have to be normalized in the head.
[[nodiscard]] float direction_degrees(glm::vec2 dir) {
    if (glm::length(dir) < 1e-6f) return 0.0f;
    return glm::degrees(std::atan2(dir.y, dir.x));
}

[[nodiscard]] glm::vec2 direction_from_degrees(float degrees) {
    float r = glm::radians(degrees);
    return {std::cos(r), std::sin(r)};
}

/// What a contributor's row in the list says. By what it holds rather than by
/// a name, for the reason the trains are: "the long swell" is how an author
/// looks for it, and it is what the row would have been called anyway.
[[nodiscard]] std::string contributor_label(const GerstnerContributor& gerstner) {
    if (gerstner.waves.empty()) return "Gerstner \xc2\xb7 no trains";
    float longest = 0.0f;
    float tallest = 0.0f;
    for (const auto& wave : gerstner.waves) {
        longest = std::max(longest, wave.wavelength);
        tallest = std::max(tallest, wave.amplitude * gerstner.amplitude_scale);
    }
    char label[96];
    std::snprintf(label, sizeof(label), "Gerstner \xc2\xb7 %zu trains, to %.0f m long, %.2f m high",
                  gerstner.waves.size(), longest, tallest);
    return label;
}

[[nodiscard]] std::string contributor_label(const FftContributor& fft) {
    char label[96];
    std::snprintf(label, sizeof(label), "Simulated \xc2\xb7 %.0f m/s wind, %.2f m high, %.0f m patch",
                  fft.wind_speed, water::significant_amplitude(fft), fft.patch_size);
    return label;
}

} // namespace

WaterEditorContext::WaterEditorContext(WaterEditorDeps deps) : deps_{deps} {}

ftk::EditorContext::DockPreset WaterEditorContext::dock_preset() const {
    return {.preview = {"Preview"}, .inspector = {"Sea", "Waves"}};
}

void WaterEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Preview");
    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

std::vector<ViewportRenderRequest> WaterEditorContext::build_render_requests() {
    // The sky, not the studio backdrop the material and VFX editors use. A
    // sea is lit by what is above it and reflects it — the Fresnel term reads
    // the sky directly — so previewing one against a grey studio card would
    // show a material the sea never has.
    return viewport_manager_.build_render_requests();
}

void WaterEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

void WaterEditorContext::open_water(const std::string& path) {
    water_path_ = path;
    water_abs_path_ = deps_.project_mgr != nullptr && deps_.project_mgr->active() != nullptr
        ? deps_.project_mgr->active()->resolve(path)
        : path;
    name_ = std::filesystem::path(path).stem().string();

    auto loaded = load_fjwater_file(water_abs_path_);
    if (!loaded) {
        FTK_CORE_ERROR("Water editor: {}", loaded.error());
        loaded_ = false;
        return;
    }
    water_ = std::move(*loaded);
    loaded_ = true;
    document_opened();
    // Open on the wind the field was authored at, so the first thing shown is
    // the sea as tuned rather than one the response has already moved.
    selected_contributor_ = 0;
    fold_stale_ = true;
    wind_fold_stale_ = true;
    if (const auto* sea = selected_gerstner()) {
        preview_wind_speed_ = sea->wind.reference_speed;
        preview_wind_heading_ = direction_degrees(sea->generator.wind_direction);
    }

    setup_preview();
    // Stand off and look along the sea rather than down at it: a wave
    // field is read by how its crests march past, which is invisible from
    // overhead.
    viewport_manager_.frame({0.0f, 0.0f, 0.0f}, 90.0f);
}

void WaterEditorContext::setup_preview() {
    preview_scene_.clear();

    auto node = std::make_unique<SceneNode>();
    node->name = "water_preview";
    auto& wc = node->components.add<WaterComponent>();
    wc.water_path = water_path_;
    wc.water = water_;
    wc.water_loaded = loaded_;
    // Big enough that the longest trains have room to read as a procession,
    // and tessellated for the shortest one the field carries.
    wc.size = 500.0f;
    wc.patches_per_side = 46;

    preview_node_id_ = preview_scene_.add_node(std::move(node));
    load_preview_material();
    // What the renderer actually reads. Nothing else will ever derive it for
    // this scene, so without it the preview opens on an empty field — a still
    // plane — until the first edit pushes one through.
    apply_preview_wind();
    preview_scene_.compute_world_matrices(nullptr);
}

void WaterEditorContext::apply_preview_wind() {
    auto* node = preview_scene_.find(preview_node_id_);
    if (node == nullptr) return;
    auto* water = node->components.get<WaterComponent>();
    if (water == nullptr) return;

    // The same derivation the environment runs over a placed sea, from the
    // editor's own wind instead of the weather's. `apply_wind` needs a field
    // and a wind and nothing else, so the preview does not need a weather
    // system to show what these dials do — and without this the four wind
    // responses move sliders that the viewport never answers.
    //
    // No lag through `advance_sea_wind` here: that exists so a front cannot
    // heave the ocean as it arrives, and a dial being dragged wants the sea
    // it asks for rather than the sea five seconds from now.
    const float heading = glm::radians(preview_wind_heading_);
    const glm::vec2 wind = glm::vec2(std::cos(heading), std::sin(heading))
                         * preview_wind_speed_;
    water->effective_water = preview_derivation_.derive(water_, wind, deps_.thread_pool);
}

void WaterEditorContext::load_preview_material() {
    auto* node = preview_scene_.find(preview_node_id_);
    if (node == nullptr) return;
    auto* water = node->components.get<WaterComponent>();
    if (water == nullptr) return;

    // Cleared first, so a path that fails to load leaves the engine's own
    // shader drawing rather than whatever was picked before it.
    water->material_path.clear();
    water->material_handle = {};
    water->material = nullptr;
    // A script's override clones the material it was made from, so it cannot
    // outlive a change of material.
    water->instance.reset();

    if (water_.preview_material.empty() || deps_.resource_manager == nullptr) return;

    const Project* project = deps_.project_mgr != nullptr ? deps_.project_mgr->active()
                                                          : nullptr;
    auto resolved = base_path::resolve_asset(water_.preview_material, project);
    auto handle = deps_.resource_manager->load_material_asset(resolved, project);
    if (!handle) {
        FTK_CORE_ERROR("Water editor: {}", handle.error());
        return;
    }
    water->material_path = water_.preview_material;
    water->material_handle = *handle;
    water->material = deps_.resource_manager->get(*handle);
}

std::string WaterEditorContext::document() const {
    return loaded_ ? fjwater_text(water_) : std::string{};
}

ftk::Result<> WaterEditorContext::write() {
    if (!loaded_) return ftk::make_error("no water is open");
    if (auto written = save_fjwater_file(water_abs_path_, water_); !written) return written;
    return {};
}

ftk::Result<> WaterEditorContext::restore(const std::string& text) {
    auto field = fjwater_from_text(text, water_abs_path_);
    if (!field) return ftk::make_error(field.error());
    const bool material_changed = field->preview_material != water_.preview_material;
    water_ = std::move(*field);
    if (selected_contributor_ >= water_.contributors.size()) selected_contributor_ = 0;
    if (material_changed) load_preview_material();
    push_live();
    return {};
}

GerstnerContributor* WaterEditorContext::selected_gerstner() {
    if (selected_contributor_ >= water_.contributors.size()) return nullptr;
    return std::get_if<GerstnerContributor>(&water_.contributors[selected_contributor_]);
}

FftContributor* WaterEditorContext::selected_simulated() {
    if (selected_contributor_ >= water_.contributors.size()) return nullptr;
    return std::get_if<FftContributor>(&water_.contributors[selected_contributor_]);
}

void WaterEditorContext::push_live() {
    fold_stale_ = true;
    wind_fold_stale_ = true;

    // A water node holds its own copy of the field it was loaded with, and
    // the pass draws from that copy — so a sea does not change because the
    // file did. Hand every node that names this asset the edited field, and
    // the viewport shows the sea being shaped rather than the one last
    // saved.
    // The editor's own sea first: it is what the viewport shows.
    if (auto* node = preview_scene_.find(preview_node_id_)) {
        if (auto* water = node->components.get<WaterComponent>()) {
            water->water = water_;
            water->water_loaded = true;
        }
    }
    // The preview scene has no weather to blow through it, so the editor
    // derives the effective field itself from its own wind dial.
    apply_preview_wind();

    if (deps_.scene == nullptr) return;
    const Project* project = deps_.project_mgr != nullptr ? deps_.project_mgr->active()
                                                          : nullptr;
    for (const auto& node : deps_.scene->objects()) {
        auto* water = node->components.get<WaterComponent>();
        if (water == nullptr) continue;
        // Compare where the references land, not how they are written: a
        // saved scene stores an asset id while the editor was handed a path,
        // and the two name the same file.
        if (base_path::resolve_asset(water->water_path, project) != water_abs_path_) {
            continue;
        }
        water->water = water_;
        water->water_loaded = true;
        // Shown at once rather than waiting a frame for the weather to
        // rederive it; a scene with wind overwrites this on its next tick.
        // Through the node's own derivation, so the sea it last drew is kept
        // where the edit did not change it.
        water->effective_water = water->derivation.derive(water_, {0.0f, 0.0f}, deps_.thread_pool);
    }
}

void WaterEditorContext::draw(float dt) {
    preview_scene_.compute_world_matrices(nullptr);
    name_preview(viewport_manager_);
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    draw_generator_panel();
    draw_wave_list_panel();
    draw_shared_panels(dt);
}

void WaterEditorContext::draw_generator_panel() {
    auto panel = ftk::ui::Panel(icon::water, ctx_title("Sea").c_str());
    if (!panel) return;
    if (!loaded_) {
        ftk::ui::empty_state(icon::warning, "The file couldn't be read", "The console says why");
        return;
    }

    // What the field is being looked at through. Crest steepness and foam
    // read differently under every material, so tuning a sea against the
    // engine's default and placing it under the game's is tuning it blind.
    InspectorContext ictx{};
    ictx.resource_manager = deps_.resource_manager;
    ictx.project_mgr = deps_.project_mgr;
    ictx.content_browser = deps_.content_browser;
    ictx.icons = deps_.icon_cache;
    if (auto t = ftk::ui::PropertyTable("##preview_material")) {
        static const std::string mat_exts[] = {".fjmat"};
        const auto* mat_type = AssetTypeRegistry::instance().find_by_name("Material");
        ftk::ui::row("Preview material",
                "The material this field is previewed through, saved with the .fjwater as an authoring "
                "setting: a placed body of water takes its material from its own component. Unset draws "
                "the engine's water shader.",
                [&] {
                    if (asset_slot({.label = "Preview material", .id = "##preview_material", .extensions = mat_exts,
                                    .ctx = &ictx, .asset_type = mat_type,
                                    .shader_types = only_shader_type(ShaderType::Water)},
                                   water_.preview_material)) {
                        load_preview_material();
                    }
                });
    }
    // A material whose shader is not water has no surface pipeline and would
    // draw nothing; say so here rather than leaving an unchanged preview to
    // be puzzled over.
    if (auto* node = preview_scene_.find(preview_node_id_)) {
        if (auto* water = node->components.get<WaterComponent>();
            water != nullptr && water->material != nullptr &&
            water->material->shader_type() != ShaderType::Water) {
            const std::string text = "Its shader is shader_type " +
                                     std::string(shader_type_name(water->material->shader_type())) +
                                     "; water needs shader_type water";
            ftk::ui::status(ftk::ui::Severity::Warning, text.c_str());
        }
    }

    draw_contributor_list();
    draw_preview_wind();
    draw_measures();

    if (water_.contributors.empty()) return;
    if (FftContributor* simulated = selected_simulated()) {
        draw_simulated_editor(*simulated);
        return;
    }
    GerstnerContributor* sea = selected_gerstner();
    if (sea == nullptr) return;

    auto& g = sea->generator;
    ftk::ui::Edit generated;
    ftk::ui::section("Generate", icon::water);
    if (auto t = ftk::ui::PropertyTable("##generate")) {
        ftk::ui::row("Wind direction", [&] {
            float wind_deg = direction_degrees(g.wind_direction);
            const auto edit = ftk::ui::slider("##wind", wind_deg, -180.0f, 180.0f, ftk::ui::Unit::Degrees, "%.0f");
            if (edit.changed) g.wind_direction = direction_from_degrees(wind_deg);
            generated |= edit;
        });
        ftk::ui::row("Swell", "Height of the largest train. The rest fall away from it.", [&] {
            generated |= ftk::ui::slider("##swell", g.swell, 0.0f, 6.0f, ftk::ui::Unit::Metres);
        });
        ftk::ui::row("Chop", "How far the crests pinch. 0 is a sum of sines.", [&] {
            generated |= ftk::ui::slider("##chop", g.chop, 0.0f, 1.0f);
        });
        ftk::ui::row("Longest wavelength", [&] {
            generated |= ftk::ui::slider("##longest", g.longest_wavelength, 2.0f, 200.0f, ftk::ui::Unit::Metres, "%.1f");
        });
        ftk::ui::row("Spread",
                "How far the trains fan either side of the wind. 0 reads as a canal rather than open sea; a "
                "swell that has travelled stays inside about 10.",
                [&] { generated |= ftk::ui::slider("##spread", g.spread_degrees, 0.0f, 90.0f, ftk::ui::Unit::Degrees, "%.0f"); });
        ftk::ui::row("Wavelength falloff",
                "How much shorter each train is than the last. Low buries the long waves under chop, which "
                "reads as a wind sea; high keeps them and the surface rolls.",
                [&] { generated |= ftk::ui::slider("##wavelength_falloff", g.wavelength_falloff, 0.30f, 0.95f); });
        ftk::ui::row("Amplitude falloff",
                "How much lower each train is than the last. Below the wavelength falloff the short waves fade "
                "out and the swell reads smooth.",
                [&] { generated |= ftk::ui::slider("##amplitude_falloff", g.amplitude_falloff, 0.30f, 0.95f); });
        ftk::ui::row("Trains", [&] {
            int count = static_cast<int>(g.wave_count);
            const auto edit = ftk::ui::slider_int("##trains", count, 1, static_cast<int>(MAX_WATER_WAVES));
            if (edit.changed) {
                g.wave_count = static_cast<std::uint32_t>(std::clamp(count, 1, static_cast<int>(MAX_WATER_WAVES)));
            }
            generated |= edit;
        });
        ftk::ui::row("", [&] {
            if (ftk::ui::action(icon::reload, "Generate waves")) generated.changed = true;
            ImGui::SetItemTooltip("Fills the wave list from the numbers above, overwriting it: a starting shape, "
                                  "and the list in Waves is the asset. Moving a number above does the same.");
        });
    }
    // Moving a generator slider regenerates as it is dragged. The generator
    // is deterministic, so this is the same list the button would make, and
    // shaping a sea by watching it beats shaping it by pressing a button.
    if (generated.changed) {
        sea->waves = generate_water_waves(g);
        push_live();
    }

    ftk::ui::section("All of its trains", icon::water);
    if (auto t = ftk::ui::PropertyTable("##scales")) {
        ftk::ui::Edit edit;
        ftk::ui::row("Amplitude scale",
                "Multiplies every train of this contributor. Where calm becomes storm without touching a "
                "single wave.",
                [&] { edit |= ftk::ui::slider("##amplitude_scale", sea->amplitude_scale, 0.0f, 4.0f, ftk::ui::Unit::Times); });
        ftk::ui::row("Speed scale", [&] {
            edit |= ftk::ui::slider("##speed_scale", sea->speed_scale, 0.0f, 3.0f, ftk::ui::Unit::Times);
        });
        ftk::ui::row("Steepness headroom",
                "How much of the fold limit the trains share. 1 can never fold whatever they do; past the "
                "measured ceiling the sharpest crests turn inside out, which reads as dark facets in the foam.",
                [&] { edit |= ftk::ui::slider("##headroom", sea->steepness_headroom, 0.5f, 4.0f); });
        draw_fold_readout();
        if (edit.changed) push_live();
    }

    ftk::ui::section("Wind response", icon::weather);
    if (auto t = ftk::ui::PropertyTable("##wind_response")) {
        ftk::ui::Edit edit;
        ftk::ui::row("Answers the weather",
                "Whether the weather's wind reaches these trains. Off for a pool, a canal, anything indoors, "
                "and for a swell that arrived from a storm somewhere else.",
                [&] { edit |= ftk::ui::checkbox("##answers", sea->wind.enabled); });
        if (sea->wind.enabled) {
            ftk::ui::row("Reference wind",
                    "The wind this sea is authored at. At this speed nothing below changes anything, so it is "
                    "the sea you are looking at now.",
                    [&] {
                        edit |= ftk::ui::slider("##reference", sea->wind.reference_speed, 0.5f, 30.0f,
                                           ftk::ui::Unit::MetresPerSecond, "%.1f");
                    });
            ftk::ui::row("Height at double wind", [&] {
                edit |= ftk::ui::slider("##height_response", sea->wind.amplitude_response, 0.0f, 4.0f, ftk::ui::Unit::Times);
            });
            ftk::ui::row("Headroom at double wind",
                    "Wind sharpens a sea as well as raising it. This multiplies the fold headroom, so a sea tuned "
                    "near its ceiling will fold in a storm.",
                    [&] {
                        edit |= ftk::ui::slider("##headroom_response", sea->wind.steepness_response, 0.0f, 3.0f,
                                           ftk::ui::Unit::Times);
                    });
            ftk::ui::row("Turns onto wind",
                    "How far the wind combs the trains around. Only the short ones turn: a long swell holds the "
                    "heading the storm that raised it gave it.",
                    [&] { edit |= ftk::ui::slider("##turn", sea->wind.direction_response, 0.0f, 1.0f); });
        }
        if (edit.changed) push_live();
    }

    // A response is quoted at double the reference wind, which is a sea
    // nobody is looking at while they tune. Show what it comes to at the
    // winds that actually blow, folding measured rather than warned about.
    if (sea->wind.enabled && !sea->waves.empty()) {
        ftk::ui::subheading("At a given wind");
        if (auto t = ftk::ui::PropertyTable("##at_wind")) {
            // Measured when the sea has changed and the mouse has let go of
            // it, like the readout above and for the same reason: five
            // whole-sea measurements are too slow to take mid-drag.
            const bool remeasure = wind_fold_stale_ && !ImGui::IsAnyItemActive();
            if (remeasure) wind_fold_stale_ = false;
            std::size_t row = 0;
            const float reference = std::max(sea->wind.reference_speed, 0.1f);
            for (float factor : {0.0f, 0.5f, 1.0f, 1.5f, 2.0f}) {
                const float speed = reference * factor;
                // Height and headroom are this contributor's own answer;
                // whether the surface folds is a question about the whole sea
                // under that wind, every contributor answering it.
                const GerstnerContributor blown_sea = water::apply_wind(*sea, {speed, 0.0f});
                if (remeasure) {
                    wind_folded_share_[row] = water::folded_share(water::apply_wind(water_, {speed, 0.0f}));
                }
                const float pct = 100.0f * wind_folded_share_[row];
                ++row;
                char label[32];
                std::snprintf(label, sizeof(label), "%.1f m/s", speed);
                char text[96];
                if (pct <= 0.0f) {
                    std::snprintf(text, sizeof(text), "%.2f\xc3\x97 high, headroom %.2f, no folding",
                                  blown_sea.amplitude_scale, blown_sea.steepness_headroom);
                } else {
                    std::snprintf(text, sizeof(text), "%.2f\xc3\x97 high, headroom %.2f, %.1f%% folds",
                                  blown_sea.amplitude_scale, blown_sea.steepness_headroom, pct);
                }
                ftk::ui::row(label, [&] {
                    if (pct <= 0.0f) {
                        ftk::ui::readout(text);
                    } else {
                        ftk::ui::status(ftk::ui::Severity::Warning, text);
                    }
                });
            }
        }
    }
}

void WaterEditorContext::draw_contributor_list() {
    ftk::ui::section("Contributors", icon::water);
    std::uint32_t gerstner_count = 0;
    bool has_simulated = false;
    for (const auto& contributor : water_.contributors) {
        if (std::holds_alternative<GerstnerContributor>(contributor)) ++gerstner_count;
        if (std::holds_alternative<FftContributor>(contributor)) has_simulated = true;
    }

    // Their motions add, so order changes nothing about the sea; it is only
    // the order they are kept in. The selected one is what the sections
    // below edit.
    bool changed = false;
    std::optional<std::size_t> remove;
    {
        ftk::ui::ListEditor list("##contributors", water_.contributors.size());
        for (std::size_t i = 0; i < water_.contributors.size(); ++i) {
            if (!list.begin_item(i, i == selected_contributor_)) continue;
            const std::string label = std::visit(
                [](const auto& kind) { return contributor_label(kind); }, water_.contributors[i]);
            ImGui::TextUnformatted(label.c_str());
            if (list.end_item()) remove = i;
            if (list.clicked()) selected_contributor_ = i;
        }
        if (water_.contributors.empty()) list.empty("Flat water. Add a contributor to give it waves");
        if (list.add_button("Add contributor")) ImGui::OpenPopup("##add_contributor");
        // Inside the list's ID scope, where the button opened it.
        if (ImGui::BeginPopup("##add_contributor")) {
            char gerstner_limit[64];
            std::snprintf(gerstner_limit, sizeof(gerstner_limit), "A field sums at most %u of them",
                          MAX_GERSTNER_CONTRIBUTORS);
            if (ftk::ui::menu_item({.icon = icon::water, .label = "Gerstner trains",
                               .enabled = gerstner_count < MAX_GERSTNER_CONTRIBUTORS,
                               .disabled_reason = gerstner_limit})) {
                // Born with the generator's sea rather than empty, so adding one
                // shows something to shape instead of a row that does nothing yet.
                GerstnerContributor added;
                added.waves = generate_water_waves(added.generator);
                water_.contributors.emplace_back(std::move(added));
                selected_contributor_ = water_.contributors.size() - 1;
                changed = true;
            }
            if (ftk::ui::menu_item({.icon = icon::simulated, .label = "Simulated sea", .enabled = !has_simulated,
                               .disabled_reason = "One to a field: a simulation is a patch of water carried forward "
                                                  "every frame, and the world runs one"})) {
                water_.contributors.emplace_back(FftContributor{});
                selected_contributor_ = water_.contributors.size() - 1;
                changed = true;
            }
            ImGui::EndPopup();
        }
        if (auto move = list.moved()) {
            const auto [from, to] = *move;
            auto first = water_.contributors.begin();
            if (from < to) {
                std::rotate(first + static_cast<std::ptrdiff_t>(from), first + static_cast<std::ptrdiff_t>(from) + 1,
                            first + static_cast<std::ptrdiff_t>(to));
            } else {
                std::rotate(first + static_cast<std::ptrdiff_t>(to), first + static_cast<std::ptrdiff_t>(from),
                            first + static_cast<std::ptrdiff_t>(from) + 1);
            }
            selected_contributor_ = ftk::ui::index_after_move(selected_contributor_, from, to);
            changed = true;
        }
    }
    if (remove) {
        water_.contributors.erase(water_.contributors.begin() + static_cast<std::ptrdiff_t>(*remove));
        // Stay where the list was being worked on: the one that slid into
        // this place, or the new last when the last was removed.
        if (selected_contributor_ > *remove) --selected_contributor_;
        if (selected_contributor_ >= water_.contributors.size() && !water_.contributors.empty()) {
            selected_contributor_ = water_.contributors.size() - 1;
        }
        changed = true;
    }
    if (changed) {
        selected_wave_ = -1;
        push_live();
    }
}

void WaterEditorContext::draw_fold_readout() {
    // What the current settings cost, measured rather than asserted: how much
    // of the surface has folded through itself. A number beats a warning here,
    // because a little folding at a distance may be a trade worth making.
    //
    // Of the whole sea, every contributor together, since the surface folds on
    // their sum: a simulated sea pinched by its choppiness folds as surely as
    // Gerstner trains pushed past their headroom, and one contributor's dial is
    // only its share of that. Measuring a simulated sea takes tens of
    // milliseconds, so it waits for the mouse to let go of whatever is being
    // dragged and the last number stands until then.
    if (fold_stale_ && !ImGui::IsAnyItemActive()) {
        folded_share_ = water::folded_share(water_);
        fold_stale_ = false;
    }
    const float pct = 100.0f * folded_share_;
    ftk::ui::row("Folding",
            "Of the whole sea: every contributor summed, the simulated one included. Folded crests are where "
            "foam gathers; some of it is what a rough sea looks like.",
            [&] {
                char text[64];
                if (pct <= 0.0f) {
                    ftk::ui::readout(fold_stale_ ? "None (measured on release)" : "None");
                } else {
                    std::snprintf(text, sizeof(text), fold_stale_ ? "%.2f%% of the surface (measured on release)"
                                                                  : "%.2f%% of the surface", pct);
                    ftk::ui::status(ftk::ui::Severity::Warning, text);
                }
            });
}

void WaterEditorContext::draw_simulated_editor(FftContributor& sea) {
    ftk::ui::Edit edit;

    ftk::ui::section("Wind", icon::weather);
    if (auto t = ftk::ui::PropertyTable("##sim_wind")) {
        ftk::ui::row("Kind",
                "A sea raised from a wind-driven spectrum and simulated on the GPU. It is drawn and floated on "
                "only through its strongest waves, set under What floats on it.",
                [&] { ftk::ui::readout("Simulated sea"); });
        ftk::ui::row("Answers the weather",
                "On, the weather's wind replaces the two below, late, so a front builds the sea over its "
                "passage. Off, this sea is raised by its own wind whatever the weather does.",
                [&] { edit |= ftk::ui::checkbox("##answers", sea.answers_weather); });
        ftk::ui::row("Heading", [&] {
            float heading = direction_degrees(sea.wind_direction);
            const auto moved = ftk::ui::slider("##heading", heading, -180.0f, 180.0f, ftk::ui::Unit::Degrees, "%.0f");
            if (moved.changed) sea.wind_direction = direction_from_degrees(heading);
            edit |= moved;
        });
        ftk::ui::row("Speed", "8 is a fresh breeze, 15 a near gale, 25 a storm.", [&] {
            edit |= ftk::ui::slider("##speed", sea.wind_speed, 0.0f, 35.0f, ftk::ui::Unit::MetresPerSecond, "%.1f");
        });
        if (sea.answers_weather) {
            ftk::ui::hint("Under weather these are the sea with no weather; here the preview wind stands in");
        }
        ftk::ui::row("Fetch",
                "How far the wind has blown over open water. Short is a lake's steep chop, long is the open "
                "ocean's heavy swell under the same wind.",
                [&] {
                    float fetch_km = sea.fetch / 1000.0f;
                    const auto moved = ftk::ui::slider("##fetch", fetch_km, 0.5f, 500.0f, ftk::ui::Unit::Kilometres, "%.1f", true);
                    if (moved.changed) sea.fetch = fetch_km * 1000.0f;
                    edit |= moved;
                });
    }

    ftk::ui::section("Stylisation", icon::water);
    if (auto t = ftk::ui::PropertyTable("##stylisation")) {
        ftk::ui::row("Height", [&] { edit |= ftk::ui::slider("##height", sea.amplitude_scale, 0.0f, 4.0f, ftk::ui::Unit::Times); });
        ftk::ui::row("Choppiness",
                "How far crests are pinched. 1 is the physical amount, which reads as gentle; past it crests "
                "sharpen until they fold and foam. The same motion a Gerstner train's steepness buys.",
                [&] { edit |= ftk::ui::slider("##choppiness", sea.choppiness, 0.0f, 3.0f); });
        ftk::ui::row("Spread",
                "The angle from the wind at which the sea carries half the energy it carries downwind. Narrow "
                "for long parallel ridges, wide for a confused sea.",
                [&] { edit |= ftk::ui::slider("##spread", sea.spread_degrees, 2.0f, 90.0f, ftk::ui::Unit::Degrees, "%.0f"); });
        ftk::ui::row("High cut",
                "Fades out waves shorter than this. The biggest stylisation control: it turns a busy correct "
                "ocean into a few big readable swells. 0 keeps everything the grid holds.",
                [&] { edit |= ftk::ui::slider("##high_cut", sea.high_cut, 0.0f, 40.0f, ftk::ui::Unit::Metres, "%.1f"); });
        draw_fold_readout();
    }

    ftk::ui::section("Simulated patch", icon::simulated);
    if (auto t = ftk::ui::PropertyTable("##patch")) {
        ftk::ui::row("Patch size",
                "The square of sea that is simulated; it tiles beyond that. It wants several of the longest waves "
                "in it, and few enough that the grid still resolves the short ones.",
                [&] {
                    edit |= ftk::ui::slider("##patch_size", sea.patch_size, 16.0f, 2000.0f, ftk::ui::Unit::Metres, "%.0f", true);
                });
        ftk::ui::row("Resolution", "Samples along each side. Four times the cost for each step up.", [&] {
            static constexpr std::uint32_t RESOLUTIONS[] = {64, 128, 256, 512, 1024};
            static constexpr const char* NAMES[] = {"64", "128", "256", "512", "1024"};
            int index = 0;
            for (int i = 0; i < 5; ++i) {
                if (RESOLUTIONS[i] == sea.resolution) index = i;
            }
            const auto picked = ftk::ui::choice("##resolution", index, NAMES);
            if (picked.changed) sea.resolution = RESOLUTIONS[index];
            edit |= picked;
        });
        ftk::ui::row("Seed", "Which sea of all the seas this spectrum describes.", [&] {
            int seed = static_cast<int>(sea.seed);
            const auto moved = ftk::ui::drag_int("##seed", seed, 0.2f, 0, 1 << 30);
            if (moved.changed) sea.seed = static_cast<std::uint32_t>(std::max(seed, 0));
            edit |= moved;
        });
    }

    ftk::ui::section("What floats on it", icon::collision);
    if (auto t = ftk::ui::PropertyTable("##floats")) {
        ftk::ui::row("Physics waves",
                "How many of this sea's strongest waves a floating body feels. The simulation is on the GPU, "
                "which physics cannot read, so buoyancy is given the strongest waves in closed form: the same "
                "waves, on the same clock. 0 floats a body on the Gerstner contributors alone. Each one is work "
                "per floating point per tick.",
                [&] {
                    int physics_waves = static_cast<int>(sea.physics_waves);
                    const auto moved = ftk::ui::slider_int("##physics_waves", physics_waves, 0, 256);
                    if (moved.changed) sea.physics_waves = static_cast<std::uint32_t>(std::max(physics_waves, 0));
                    edit |= moved;
                });
        draw_physics_waves_readout(sea);
    }

    if (edit.changed) push_live();

    // What those settings come to, since none of them is a wave to look at.
    ftk::ui::section("What it comes to", icon::info);
    if (auto t = ftk::ui::PropertyTable("##comes_to")) {
        const float peak = water::peak_frequency(sea);
        if (peak > 0.0f) {
            const float wavelength = glm::two_pi<float>() * 9.81f / (peak * peak);
            char text[64];
            std::snprintf(text, sizeof(text), "%.2f m", water::significant_amplitude(sea) * 2.0f);
            ftk::ui::row("Significant height", [&] { ftk::ui::readout(text); });
            std::snprintf(text, sizeof(text), "%.0f m, every %.1f s", wavelength, glm::two_pi<float>() / peak);
            ftk::ui::row("Dominant wave", [&] { ftk::ui::readout(text); });
            if (wavelength * 3.0f > sea.patch_size) {
                ftk::ui::hint("The patch holds under 3 of them, so they are cut short");
            }
        } else {
            ftk::ui::row("Significant height", [&] { ftk::ui::readout("No wind, no sea"); });
        }
        const float texel = sea.patch_size / static_cast<float>(std::max(sea.resolution, 1u));
        char grid[64];
        std::snprintf(grid, sizeof(grid), "%.2f m a sample, waves down to %.1f m", texel, texel * 2.0f);
        ftk::ui::row("Grid", [&] { ftk::ui::readout(grid); });
    }
}

void WaterEditorContext::draw_physics_waves_readout(const FftContributor& sea) {
    if (sea.physics_waves == 0) {
        ftk::ui::hint("A floating body does not feel this sea");
        return;
    }
    // Read off the sea the preview is actually running, so the number is about
    // the waves physics is being given and not about a second draw made for the
    // readout. Taken again only when that sea or the count changes: ranking
    // every wavevector is too much to do per frame.
    const auto* node = preview_scene_.find(preview_node_id_);
    const auto* water = node != nullptr ? node->components.get<WaterComponent>() : nullptr;
    if (water == nullptr || !water->effective_water.simulated.has_value()) return;
    const auto& running = *water->effective_water.simulated;
    if (running.spectrum == nullptr) return;

    if (running.spectrum != physics_readout_spectrum_ ||
        sea.physics_waves != physics_readout_count_) {
        const water::StrongestWaves taken =
            water::strongest_waves(*running.spectrum, sea.physics_waves);
        physics_readout_spectrum_ = running.spectrum;
        physics_readout_count_ = sea.physics_waves;
        physics_height_share_ = taken.height_share;
        physics_shortest_wave_ = taken.shortest_wavelength;
    }
    char text[80];
    std::snprintf(text, sizeof(text), "%.0f%% of its height, waves down to %.1f m", physics_height_share_ * 100.0f,
                  physics_shortest_wave_);
    ftk::ui::row("A body feels",
            "The strongest waves carry most of a sea's height. What a body floating on them misses is "
            "everything shorter than the shortest of them: about right for a hull that long or longer, which "
            "averages out chop shorter than itself, and visibly wrong for a buoy or a swimmer.",
            [&] { ftk::ui::readout(text); });
}

void WaterEditorContext::draw_preview_wind() {
    // Shown once anything in the sea answers the wind; until then these would
    // be dials with nothing to answer them.
    const GerstnerContributor* answering = nullptr;
    bool any_answers = false;
    for (const auto& contributor : water_.contributors) {
        if (const auto* gerstner = std::get_if<GerstnerContributor>(&contributor)) {
            if (gerstner->wind.enabled && answering == nullptr) answering = gerstner;
            any_answers |= gerstner->wind.enabled;
        } else if (const auto* fft = std::get_if<FftContributor>(&contributor)) {
            any_answers |= fft->answers_weather;
        }
    }
    if (!any_answers) return;

    // The wind blowing on the preview, which is the editor's and not the
    // asset's: a .fjwater says how a sea answers wind, never what wind it
    // sits in. One wind for the whole preview, as the weather's is for a
    // placed sea, with every contributor answering it by its own response.
    ftk::ui::section("Preview wind", icon::weather);
    if (auto t = ftk::ui::PropertyTable("##preview_wind")) {
        ftk::ui::row("Speed",
                "What the preview is blown at, to watch the wind responses. Not saved: a placed sea takes its "
                "wind from the weather.",
                [&] {
                    if (ftk::ui::slider("##preview_speed", preview_wind_speed_, 0.0f, 40.0f, ftk::ui::Unit::MetresPerSecond,
                                   "%.1f").changed) {
                        apply_preview_wind();
                    }
                });
        ftk::ui::row("Heading", [&] {
            if (ftk::ui::slider("##preview_heading", preview_wind_heading_, -180.0f, 180.0f, ftk::ui::Unit::Degrees, "%.0f")
                    .changed) {
                apply_preview_wind();
            }
        });
        // Back to the sea as authored, which only a Gerstner contributor has a
        // wind for: a simulated one is whatever sea the wind it is given raises.
        const GerstnerContributor* selected = selected_gerstner();
        const GerstnerContributor* reference = selected != nullptr && selected->wind.enabled ? selected : answering;
        if (reference != nullptr) {
            ftk::ui::row("", [&] {
                if (ftk::ui::action(icon::revert, "Reference wind")) {
                    preview_wind_speed_ = reference->wind.reference_speed;
                    apply_preview_wind();
                }
                ImGui::SetItemTooltip("Back to the reference wind of the selected contributor, which is that "
                                      "contributor as authored");
            });
        }
    }
}

void WaterEditorContext::draw_measures() {
    // Of the whole sea as authored, every contributor together: what an
    // author steers by is the water, not one list of it.
    float longest = 0.0f;
    float longest_speed_scale = 1.0f;
    water::for_each_summed_train(water_, [&](std::uint32_t, const GerstnerContributor& gerstner,
                                             const WaterWave& wave) {
        if (wave.wavelength > longest) {
            longest = wave.wavelength;
            longest_speed_scale = gerstner.speed_scale * wave.speed;
        }
    });
    if (longest <= 0.0f) return;

    ftk::ui::section("Measures", icon::info);
    if (auto t = ftk::ui::PropertyTable("##measures")) {
        char text[64];
        std::snprintf(text, sizeof(text), "%.2f m", water::wave_height(water_) * 2.0f);
        ftk::ui::row("Crest to trough", "With every crest of every contributor aligned, which a sea only rarely arranges.",
                [&] { ftk::ui::readout(text); });
        std::snprintf(text, sizeof(text), "%.1f m", longest);
        ftk::ui::row("Longest train", [&] { ftk::ui::readout(text); });
        // The period is what says whether a sea rolls. Wind chop comes in at
        // two to five seconds and reads as busy; swell that has travelled
        // from a storm arrives at eight to fourteen and reads as a procession
        // of ridges. The wavelength decides it, so this is a consequence
        // rather than a setting, but it is the number to steer by.
        const float omega = water::wave_frequency(longest) * longest_speed_scale;
        if (omega > 1e-4f) {
            const float period = glm::two_pi<float>() / omega;
            const char* reads = period < 6.0f ? "wind chop" : (period < 8.0f ? "building" : "rolling swell");
            std::snprintf(text, sizeof(text), "%.1f s, reads as %s", period, reads);
            ftk::ui::row("Longest period", [&] { ftk::ui::readout(text); });
        }
        // The shortest train is what decides how finely the surface has to be
        // tessellated: under about four vertices per wavelength it cannot be
        // drawn and comes out as noise instead of chop.
        const float shortest = water::shortest_wavelength(water_);
        std::snprintf(text, sizeof(text), "%.1f m", shortest);
        ftk::ui::row("Shortest train", [&] { ftk::ui::readout(text); });
        std::snprintf(text, sizeof(text), "Needs about %.1f m between vertices", shortest / 4.0f);
        ftk::ui::hint(text);
    }
}

void WaterEditorContext::draw_wave_list_panel() {
    auto panel = ftk::ui::Panel(icon::curve, ctx_title("Waves").c_str());
    if (!panel) return;
    if (!loaded_) return;

    GerstnerContributor* sea = selected_gerstner();
    if (sea == nullptr) {
        ftk::ui::empty_state(icon::water, "No trains here", "Trains belong to a Gerstner contributor; select one in Sea");
        return;
    }

    char count[48];
    std::snprintf(count, sizeof(count), "%zu of %u", sea->waves.size(), MAX_WATER_WAVES);
    if (auto t = ftk::ui::PropertyTable("##train_count")) {
        ftk::ui::row("Trains", [&] { ftk::ui::readout(count); });
    }

    // Each train is named by its size rather than its index: which one is
    // "the big swell" is the question being asked of the list.
    bool changed = false;
    std::optional<std::size_t> remove;
    {
        ftk::ui::ListEditor list("##waves", sea->waves.size(), {.reorder = false, .opens = true});
        for (std::size_t i = 0; i < sea->waves.size(); ++i) {
            auto& wave = sea->waves[i];
            const bool open = selected_wave_ == static_cast<int>(i);
            if (!list.begin_item(i, open)) continue;
            if (open) {
                if (auto t = ftk::ui::PropertyTable("##wave")) {
                    ftk::ui::Edit edit;
                    ftk::ui::row("Direction", [&] {
                        float deg = direction_degrees(wave.direction);
                        const auto moved = ftk::ui::slider("##direction", deg, -180.0f, 180.0f, ftk::ui::Unit::Degrees, "%.0f");
                        if (moved.changed) wave.direction = direction_from_degrees(deg);
                        edit |= moved;
                    });
                    ftk::ui::row("Wavelength", [&] {
                        edit |= ftk::ui::slider("##wavelength", wave.wavelength, 0.5f, 200.0f, ftk::ui::Unit::Metres, "%.1f");
                    });
                    ftk::ui::row("Amplitude", [&] {
                        edit |= ftk::ui::slider("##amplitude", wave.amplitude, 0.0f, 6.0f, ftk::ui::Unit::Metres);
                    });
                    ftk::ui::row("Steepness", [&] { edit |= ftk::ui::slider("##steepness", wave.steepness, 0.0f, 1.0f); });
                    ftk::ui::row("Speed", [&] { edit |= ftk::ui::slider("##speed", wave.speed, 0.0f, 3.0f, ftk::ui::Unit::Times); });
                    // A train travels at the speed its wavelength implies, so
                    // the period is a consequence rather than a setting, worth
                    // showing because two trains with close periods beat
                    // against each other.
                    const float omega = water::wave_frequency(wave.wavelength) * wave.speed * sea->speed_scale;
                    if (omega > 1e-4f) {
                        char period[32];
                        std::snprintf(period, sizeof(period), "%.1f s", glm::two_pi<float>() / omega);
                        ftk::ui::row("Period", [&] { ftk::ui::readout(period); });
                    }
                    changed |= edit.changed;
                }
            } else {
                char summary[96];
                std::snprintf(summary, sizeof(summary), "%.1f m long \xc2\xb7 %.2f m high \xc2\xb7 %.0f\xc2\xb0",
                              wave.wavelength, wave.amplitude, direction_degrees(wave.direction));
                ImGui::TextUnformatted(summary);
            }
            if (list.end_item()) remove = i;
            if (list.clicked()) selected_wave_ = open ? -1 : static_cast<int>(i);
        }
        if (sea->waves.empty()) list.empty("No trains: generate some in Sea, or add one");
        ImGui::BeginDisabled(sea->waves.size() >= MAX_WATER_WAVES);
        if (list.add_button("Add train")) {
            sea->waves.push_back(WaterWave{});
            selected_wave_ = static_cast<int>(sea->waves.size()) - 1;
            changed = true;
        }
        ImGui::EndDisabled();
    }
    if (remove) {
        sea->waves.erase(sea->waves.begin() + static_cast<std::ptrdiff_t>(*remove));
        if (selected_wave_ == static_cast<int>(*remove)) selected_wave_ = -1;
        else if (selected_wave_ > static_cast<int>(*remove)) --selected_wave_;
        changed = true;
    }
    if (changed) push_live();
}

} // namespace fjell
