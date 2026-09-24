#include "ui/water_editor_context.hpp"

#include "core/asset_type_registry.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
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
#include "ui/dockspace_layout.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
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
    if (gerstner.waves.empty()) return "Gerstner  -  no trains";
    float longest = 0.0f;
    float tallest = 0.0f;
    for (const auto& wave : gerstner.waves) {
        longest = std::max(longest, wave.wavelength);
        tallest = std::max(tallest, wave.amplitude * gerstner.amplitude_scale);
    }
    char label[96];
    std::snprintf(label, sizeof(label), "Gerstner  -  %zu trains, to %.0f m long, %.2f m high",
                  gerstner.waves.size(), longest, tallest);
    return label;
}

[[nodiscard]] std::string contributor_label(const FftContributor& fft) {
    char label[96];
    std::snprintf(label, sizeof(label), "Simulated  -  %.0f m/s wind, %.2f m high, %.0f m patch",
                  fft.wind_speed, water::significant_amplitude(fft), fft.patch_size);
    return label;
}

} // namespace

WaterEditorContext::WaterEditorContext(WaterEditorDeps deps) : deps_{deps} {}

void WaterEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Water Preview");
    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

void WaterEditorContext::shutdown() {
    viewport_manager_.release_textures();
}

std::vector<std::string> WaterEditorContext::docked_window_names() const {
    return {ctx_title("Water Preview"), ctx_title("Sea"), ctx_title("Waves"),
            ctx_title("Stats"), ctx_title("Console")};
}

void WaterEditorContext::setup_dockspace(ImGuiID main_area) {
    enum class EPanel { Preview, Sea, Waves, Stats, Console };
    using N = DockNode;
    static const auto layout = N::horizontal_split(0.58f,
        N::leaf({EPanel::Preview}),
        N::vertical_split(0.32f,
            N::leaf({EPanel::Sea}),
            N::vertical_split(0.62f,
                N::leaf({EPanel::Waves}),
                N::leaf({EPanel::Stats, EPanel::Console}))));
    build_dockspace(main_area, layout, docked_window_names());
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
        FJELL_CORE_ERROR("Water editor: {}", loaded.error());
        loaded_ = false;
        return;
    }
    water_ = std::move(*loaded);
    loaded_ = true;
    dirty_ = false;
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
    if (auto* cam = viewport_manager_.active_camera()) {
        // Stand off and look along the sea rather than down at it: a wave
        // field is read by how its crests march past, which is invisible from
        // overhead.
        cam->focus({0.0f, 0.0f, 0.0f}, 90.0f);
    }
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
        FJELL_CORE_ERROR("Water editor: {}", handle.error());
        return;
    }
    water->material_path = water_.preview_material;
    water->material_handle = *handle;
    water->material = deps_.resource_manager->get(*handle);
}

bool WaterEditorContext::save() {
    if (!loaded_) return false;
    auto written = save_fjwater_file(water_abs_path_, water_);
    if (!written) {
        FJELL_CORE_ERROR("Water editor: {}", written.error());
        return false;
    }
    dirty_ = false;
    return true;
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
    dirty_ = true;
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
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    draw_generator_panel();
    draw_wave_list_panel();
    draw_shared_panels(dt);
}

void WaterEditorContext::draw_generator_panel() {
    ImGui::Begin(ctx_title("Sea").c_str());
    if (!loaded_) {
        ImGui::TextDisabled("No .fjwater open.");
        ImGui::End();
        return;
    }

    // What the field is being looked at through. Crest steepness and foam
    // read differently under every material, so tuning a sea against the
    // engine's default and placing it under the game's is tuning it blind.
    ImGui::TextDisabled("Preview");
    InspectorContext ictx{};
    ictx.resource_manager = deps_.resource_manager;
    ictx.project_mgr = deps_.project_mgr;
    ictx.content_browser = deps_.content_browser;
    ictx.icons = deps_.icon_cache;

    static const std::string mat_exts[] = {".fjmat"};
    const auto* mat_type = AssetTypeRegistry::instance().find_by_name("Material");
    if (asset_ref_field({.label = "Material",
                         .id = "##WaterPreviewMat",
                         .extensions = mat_exts,
                         .ctx = &ictx,
                         .asset_type = mat_type,
                         .shader_types = only_shader_type(ShaderType::Water)},
                        water_.preview_material)) {
        load_preview_material();
        dirty_ = true;
    }
    ImGui::SetItemTooltip("The material this field is previewed through. Saved "
                          "with the .fjwater as an authoring setting — a placed "
                          "body of water takes its material from its own "
                          "component. Unset draws the engine's water shader.");

    // A material whose shader is not water has no surface pipeline and would
    // draw nothing; say so here rather than leaving an unchanged preview to
    // be puzzled over.
    if (auto* node = preview_scene_.find(preview_node_id_)) {
        if (auto* water = node->components.get<WaterComponent>();
            water != nullptr && water->material != nullptr &&
            water->material->shader_type() != ShaderType::Water) {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                               "shader_type %s — water needs shader_type water",
                               std::string(shader_type_name(water->material->shader_type())).c_str());
        }
    }

    ImGui::Separator();
    draw_contributor_list();
    draw_preview_wind();
    draw_measures();
    ImGui::Separator();

    if (water_.contributors.empty()) {
        ImGui::TextDisabled("Flat water. Add a contributor to give it waves.");
        ImGui::End();
        return;
    }
    if (FftContributor* simulated = selected_simulated()) {
        draw_simulated_editor(*simulated);
        ImGui::End();
        return;
    }
    GerstnerContributor* sea = selected_gerstner();
    if (sea == nullptr) {
        ImGui::TextDisabled("Select a contributor to edit it.");
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Generate");
    ImGui::TextWrapped(
        "Fills the wave list from a few numbers. Overwrites whatever is "
        "there — it is a starting shape, and the list below is the asset.");
    ImGui::Spacing();

    auto& g = sea->generator;
    bool changed = false;

    float wind_deg = direction_degrees(g.wind_direction);
    if (ImGui::SliderFloat("Wind direction", &wind_deg, -180.0f, 180.0f, "%.0f deg")) {
        g.wind_direction = direction_from_degrees(wind_deg);
        changed = true;
    }
    changed |= ImGui::SliderFloat("Swell", &g.swell, 0.0f, 6.0f, "%.2f m");
    ImGui::SetItemTooltip("Height of the largest train. The rest fall away from it.");
    changed |= ImGui::SliderFloat("Chop", &g.chop, 0.0f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("How far the crests pinch. 0 is a sum of sines.");
    changed |= ImGui::SliderFloat("Longest wavelength", &g.longest_wavelength,
                                  2.0f, 200.0f, "%.1f m");
    changed |= ImGui::SliderFloat("Spread", &g.spread_degrees, 0.0f, 90.0f, "%.0f deg");
    ImGui::SetItemTooltip("How far the trains fan either side of the wind. "
                          "0 reads as a canal rather than open sea; a swell "
                          "that has travelled stays inside about 10.");
    changed |= ImGui::SliderFloat("Wavelength falloff", &g.wavelength_falloff,
                                  0.30f, 0.95f, "%.2f");
    ImGui::SetItemTooltip("How much shorter each train is than the last. Low "
                          "buries the long waves under chop, which reads as a "
                          "wind sea; high keeps them and the surface rolls.");
    changed |= ImGui::SliderFloat("Amplitude falloff", &g.amplitude_falloff,
                                  0.30f, 0.95f, "%.2f");
    ImGui::SetItemTooltip("How much lower each train is than the last. Below "
                          "the wavelength falloff the short waves fade out "
                          "and the swell reads smooth.");

    int count = static_cast<int>(g.wave_count);
    if (ImGui::SliderInt("Trains", &count, 1, static_cast<int>(MAX_WATER_WAVES))) {
        g.wave_count = static_cast<std::uint32_t>(std::clamp(count, 1,
                                    static_cast<int>(MAX_WATER_WAVES)));
        changed = true;
    }

    ImGui::Spacing();
    if (ImGui::Button("Generate waves", ImVec2(-1, 0))) {
        sea->waves = generate_water_waves(g);
        push_live();
    }
    // Moving a generator slider regenerates as it is dragged. The generator
    // is deterministic, so this is the same list the button would make, and
    // shaping a sea by watching it beats shaping it by pressing a button.
    if (changed) {
        sea->waves = generate_water_waves(g);
        push_live();
    }

    ImGui::Separator();
    ImGui::TextDisabled("All of its trains");
    if (ImGui::SliderFloat("Amplitude scale", &sea->amplitude_scale, 0.0f, 4.0f, "%.2f")) {
        push_live();
    }
    ImGui::SetItemTooltip("Multiplies every train of this contributor. Where "
                          "calm becomes storm without touching a single wave.");
    if (ImGui::SliderFloat("Speed scale", &sea->speed_scale, 0.0f, 3.0f, "%.2f")) {
        push_live();
    }
    if (ImGui::SliderFloat("Steepness headroom", &sea->steepness_headroom,
                           0.5f, 4.0f, "%.2f")) {
        push_live();
    }
    ImGui::SetItemTooltip("How much of the fold limit the trains share. 1 can "
                          "never fold whatever they do; past the measured "
                          "ceiling the sharpest crests turn inside out, which "
                          "reads as dark facets in the foam.");

    draw_fold_readout();

    ImGui::Separator();
    ImGui::TextDisabled("Wind response");
    if (ImGui::Checkbox("Answers the weather", &sea->wind.enabled)) {
        push_live();
    }
    ImGui::SetItemTooltip("Whether the weather's wind reaches these trains. "
                          "Off for a pool, a canal, anything indoors — and for "
                          "a swell that arrived from a storm somewhere else.");
    if (sea->wind.enabled) {
        if (ImGui::SliderFloat("Reference wind", &sea->wind.reference_speed,
                               0.5f, 30.0f, "%.1f m/s")) {
            push_live();
        }
        ImGui::SetItemTooltip("The wind this sea is authored at. At this speed "
                              "nothing below changes anything, so it is the "
                              "sea you are looking at now.");
        if (ImGui::SliderFloat("Height at double wind", &sea->wind.amplitude_response,
                               0.0f, 4.0f, "%.2fx")) {
            push_live();
        }
        if (ImGui::SliderFloat("Headroom at double wind", &sea->wind.steepness_response,
                               0.0f, 3.0f, "%.2fx")) {
            push_live();
        }
        ImGui::SetItemTooltip("Wind sharpens a sea as well as raising it. This "
                              "multiplies the fold headroom, so a sea tuned "
                              "near its ceiling will fold in a storm.");
        if (ImGui::SliderFloat("Turns onto wind", &sea->wind.direction_response,
                               0.0f, 1.0f, "%.2f")) {
            push_live();
        }
        ImGui::SetItemTooltip("How far the wind combs the trains around. Only "
                              "the short ones turn: a long swell holds the "
                              "heading the storm that raised it gave it.");

        // A response is quoted at double the reference wind, which is a sea
        // nobody is looking at while they tune. Show what it comes to at the
        // winds that actually blow, folding measured rather than warned
        // about — the same reading as above, at the storm end.
        if (!sea->waves.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("At a given wind");
            if (ImGui::BeginTable("wind_preview", 4,
                                  ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("wind");
                ImGui::TableSetupColumn("height");
                ImGui::TableSetupColumn("headroom");
                ImGui::TableSetupColumn("folds");
                ImGui::TableHeadersRow();

                // Measured when the sea has changed and the mouse has let go
                // of it, like the readout above and for the same reason: five
                // whole-sea measurements are too slow to take mid-drag.
                const bool remeasure_wind_folds = wind_fold_stale_ && !ImGui::IsAnyItemActive();
                if (remeasure_wind_folds) wind_fold_stale_ = false;
                std::size_t row = 0;

                const float reference = std::max(sea->wind.reference_speed, 0.1f);
                for (float factor : {0.0f, 0.5f, 1.0f, 1.5f, 2.0f}) {
                    const float speed = reference * factor;
                    // Height and headroom are this contributor's own answer;
                    // whether the surface folds is a question about the whole
                    // sea under that wind, every contributor answering it.
                    const GerstnerContributor blown_sea =
                        water::apply_wind(*sea, {speed, 0.0f});
                    const FjWaterData blown = water::apply_wind(water_, {speed, 0.0f});

                    if (remeasure_wind_folds) {
                        wind_folded_share_[row] = water::folded_share(blown);
                    }
                    const float pct = 100.0f * wind_folded_share_[row];
                    ++row;

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f m/s", speed);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2fx", blown_sea.amplitude_scale);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", blown_sea.steepness_headroom);
                    ImGui::TableNextColumn();
                    if (pct <= 0.0f) {
                        ImGui::TextDisabled("none");
                    } else {
                        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                                           "%.1f%%", pct);
                    }
                }
                ImGui::EndTable();
            }
        }
    }

    ImGui::End();
}

void WaterEditorContext::draw_contributor_list() {
    ImGui::TextDisabled("Contributors");
    ImGui::SetItemTooltip("What moves the surface. Their motions add, so order "
                          "changes nothing about the sea — it is only the order "
                          "you keep them in.");

    for (std::size_t i = 0; i < water_.contributors.size(); ++i) {
        const std::string label = std::visit(
            [](const auto& kind) { return contributor_label(kind); }, water_.contributors[i]);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(label.c_str(), i == selected_contributor_)) {
            selected_contributor_ = i;
        }
        ImGui::PopID();
    }
    if (water_.contributors.empty()) {
        ImGui::TextDisabled("none");
    }

    const bool has_selection = selected_contributor_ < water_.contributors.size();

    std::uint32_t gerstner_count = 0;
    bool has_simulated = false;
    for (const auto& contributor : water_.contributors) {
        if (std::holds_alternative<GerstnerContributor>(contributor)) ++gerstner_count;
        if (std::holds_alternative<FftContributor>(contributor)) has_simulated = true;
    }

    ImGui::BeginDisabled(gerstner_count >= MAX_GERSTNER_CONTRIBUTORS);
    if (ImGui::SmallButton("Add Gerstner")) {
        // Born with the generator's sea rather than empty, so adding one shows
        // something to shape instead of a row that does nothing yet.
        GerstnerContributor added;
        added.waves = generate_water_waves(added.generator);
        water_.contributors.emplace_back(std::move(added));
        selected_contributor_ = water_.contributors.size() - 1;
        push_live();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Authored trains, one at a time. A field sums at most %u "
                          "Gerstner contributors.", MAX_GERSTNER_CONTRIBUTORS);

    ImGui::SameLine();
    ImGui::BeginDisabled(has_simulated);
    if (ImGui::SmallButton("Add Simulated")) {
        water_.contributors.emplace_back(FftContributor{});
        selected_contributor_ = water_.contributors.size() - 1;
        push_live();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("A sea raised from a wind-driven spectrum on the GPU. One to a "
                          "field: a simulation is a patch of water carried forward every "
                          "frame, and the world runs one.");

    ImGui::SameLine();
    ImGui::BeginDisabled(!has_selection || selected_contributor_ == 0);
    if (ImGui::SmallButton("Up")) {
        std::swap(water_.contributors[selected_contributor_],
                  water_.contributors[selected_contributor_ - 1]);
        --selected_contributor_;
        push_live();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!has_selection ||
                         selected_contributor_ + 1 >= water_.contributors.size());
    if (ImGui::SmallButton("Down")) {
        std::swap(water_.contributors[selected_contributor_],
                  water_.contributors[selected_contributor_ + 1]);
        ++selected_contributor_;
        push_live();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!has_selection);
    // Says what it is about to take, since it acts on the selection and not on
    // whichever row the eye happens to be on.
    const char* remove_label = !has_selection ? "Remove"
        : (std::holds_alternative<FftContributor>(water_.contributors[selected_contributor_])
               ? "Remove Simulated" : "Remove Gerstner");
    if (ImGui::SmallButton(remove_label)) {
        water_.contributors.erase(water_.contributors.begin()
                                  + static_cast<std::ptrdiff_t>(selected_contributor_));
        // Stay where the list was being worked on: the one that slid into
        // this place, or the new last when the last was removed.
        if (selected_contributor_ >= water_.contributors.size() &&
            !water_.contributors.empty()) {
            selected_contributor_ = water_.contributors.size() - 1;
        }
        push_live();
    }
    ImGui::EndDisabled();
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
    if (pct <= 0.0f) {
        ImGui::TextDisabled(fold_stale_ ? "no folding  (measuring on release)" : "no folding");
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                           fold_stale_ ? "%.2f%% of the surface folds  (measuring on release)"
                                       : "%.2f%% of the surface folds", pct);
    }
    ImGui::SetItemTooltip("Of the whole sea: every contributor summed, the simulated one "
                          "included. Folded crests are where foam gathers; some of it is "
                          "what a rough sea looks like.");
}

void WaterEditorContext::draw_simulated_editor(FftContributor& sea) {
    ImGui::TextWrapped("A sea raised from a wind-driven spectrum and simulated on the "
                       "GPU. It is drawn and not floated on: buoyancy rides the Gerstner "
                       "contributors and does not feel this one.");
    ImGui::Spacing();

    bool changed = false;

    ImGui::TextDisabled("The wind that raises it");
    changed |= ImGui::Checkbox("Answers the weather", &sea.answers_weather);
    ImGui::SetItemTooltip("On, the weather's wind replaces the two dials below — late, "
                          "so a front builds the sea over its passage. Off, this sea "
                          "is raised by its own wind whatever the weather does.");
    float heading = direction_degrees(sea.wind_direction);
    if (ImGui::SliderFloat("Wind heading", &heading, -180.0f, 180.0f, "%.0f deg")) {
        sea.wind_direction = direction_from_degrees(heading);
        changed = true;
    }
    changed |= ImGui::SliderFloat("Wind speed", &sea.wind_speed, 0.0f, 35.0f, "%.1f m/s");
    ImGui::SetItemTooltip("8 is a fresh breeze, 15 a near gale, 25 a storm.");
    if (sea.answers_weather) {
        ImGui::TextDisabled("under weather these are the sea with no weather, and the "
                            "preview wind stands in for it here");
    }

    float fetch_km = sea.fetch / 1000.0f;
    if (ImGui::SliderFloat("Fetch", &fetch_km, 0.5f, 500.0f, "%.1f km",
                           ImGuiSliderFlags_Logarithmic)) {
        sea.fetch = fetch_km * 1000.0f;
        changed = true;
    }
    ImGui::SetItemTooltip("How far the wind has blown over open water. Short is a lake's "
                          "steep chop, long is the open ocean's heavy swell under the "
                          "same wind.");

    ImGui::Separator();
    ImGui::TextDisabled("Stylisation");
    changed |= ImGui::SliderFloat("Height", &sea.amplitude_scale, 0.0f, 4.0f, "%.2fx");
    changed |= ImGui::SliderFloat("Choppiness", &sea.choppiness, 0.0f, 3.0f, "%.2f");
    ImGui::SetItemTooltip("How far crests are pinched. 1 is the physical amount, which "
                          "reads as gentle; past it crests sharpen until they fold and "
                          "foam. The same motion a Gerstner train's steepness buys.");
    changed |= ImGui::SliderFloat("Spread", &sea.spread_degrees, 2.0f, 90.0f, "%.0f deg");
    ImGui::SetItemTooltip("The angle from the wind at which the sea carries half the "
                          "energy it carries downwind. Narrow for long parallel ridges, "
                          "wide for a confused sea.");
    changed |= ImGui::SliderFloat("High cut", &sea.high_cut, 0.0f, 40.0f, "%.1f m");
    ImGui::SetItemTooltip("Fades out waves shorter than this. The biggest stylisation "
                          "control: it turns a busy correct ocean into a few big readable "
                          "swells. 0 keeps everything the grid holds.");
    draw_fold_readout();

    ImGui::Separator();
    ImGui::TextDisabled("The simulated patch");
    changed |= ImGui::SliderFloat("Patch size", &sea.patch_size, 16.0f, 2000.0f, "%.0f m",
                                  ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("The square of sea that is simulated; it tiles beyond that. It "
                          "wants several of the longest waves in it, and few enough that "
                          "the grid still resolves the short ones.");
    static constexpr std::uint32_t RESOLUTIONS[] = {64, 128, 256, 512, 1024};
    if (ImGui::BeginCombo("Resolution", std::to_string(sea.resolution).c_str())) {
        for (std::uint32_t option : RESOLUTIONS) {
            if (ImGui::Selectable(std::to_string(option).c_str(), option == sea.resolution)) {
                sea.resolution = option;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Samples along each side. Four times the cost for each step up.");
    int seed = static_cast<int>(sea.seed);
    if (ImGui::InputInt("Seed", &seed)) {
        sea.seed = static_cast<std::uint32_t>(std::max(seed, 0));
        changed = true;
    }
    ImGui::SetItemTooltip("Which sea of all the seas this spectrum describes.");

    ImGui::Separator();
    ImGui::TextDisabled("What floats on it");
    int physics_waves = static_cast<int>(sea.physics_waves);
    if (ImGui::SliderInt("Physics waves", &physics_waves, 0, 256)) {
        sea.physics_waves = static_cast<std::uint32_t>(std::max(physics_waves, 0));
        changed = true;
    }
    ImGui::SetItemTooltip("How many of this sea's strongest waves a floating body feels. "
                          "The simulation is on the GPU, which physics cannot read, so "
                          "buoyancy is given the strongest waves in closed form — the same "
                          "waves, on the same clock. 0 floats a body on the Gerstner "
                          "contributors alone. Each one is work per floating point per tick.");
    draw_physics_waves_readout(sea);

    if (changed) push_live();

    // What those settings come to, since none of them is a wave to look at.
    ImGui::Separator();
    ImGui::TextDisabled("What it comes to");
    const float peak = water::peak_frequency(sea);
    if (peak > 0.0f) {
        const float wavelength = glm::two_pi<float>() * 9.81f / (peak * peak);
        ImGui::Text("Significant height: %.2f m", water::significant_amplitude(sea) * 2.0f);
        ImGui::Text("Dominant wave:      %.0f m, every %.1f s", wavelength,
                    glm::two_pi<float>() / peak);
        if (wavelength * 3.0f > sea.patch_size) {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                               "the patch holds under 3 of them — they will be cut short");
        }
    } else {
        ImGui::TextDisabled("no wind, no sea");
    }
    const float texel = sea.patch_size / static_cast<float>(std::max(sea.resolution, 1u));
    ImGui::Text("Grid:               %.2f m a sample, waves down to %.1f m", texel, texel * 2.0f);
}

void WaterEditorContext::draw_physics_waves_readout(const FftContributor& sea) {
    if (sea.physics_waves == 0) {
        ImGui::TextDisabled("a floating body does not feel this sea");
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
    ImGui::Text("feels %.0f%% of its height, waves down to %.1f m",
                physics_height_share_ * 100.0f, physics_shortest_wave_);
    ImGui::SetItemTooltip("The strongest waves carry most of a sea's height. What a body "
                          "floating on them misses is everything shorter than the shortest "
                          "of them: about right for a hull that long or longer, which "
                          "averages out chop shorter than itself, and visibly wrong for a "
                          "buoy or a swimmer.");
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

    ImGui::Spacing();
    // The wind blowing on the preview, which is the editor's and not the
    // asset's: a .fjwater says how a sea answers wind, never what wind it
    // sits in. One wind for the whole preview, as the weather's is for a
    // placed sea, with every contributor answering it by its own response.
    if (ImGui::SliderFloat("Preview wind", &preview_wind_speed_, 0.0f, 40.0f, "%.1f m/s")) {
        apply_preview_wind();
    }
    ImGui::SetItemTooltip("What the preview is blown at, to watch the wind "
                          "responses. Not saved — a placed sea takes its wind "
                          "from the weather.");
    if (ImGui::SliderFloat("Preview wind heading", &preview_wind_heading_,
                           -180.0f, 180.0f, "%.0f deg")) {
        apply_preview_wind();
    }
    // Back to the sea as authored, which only a Gerstner contributor has a
    // wind for: a simulated one is whatever sea the wind it is given raises.
    const GerstnerContributor* selected = selected_gerstner();
    const GerstnerContributor* reference = selected != nullptr && selected->wind.enabled
        ? selected
        : answering;
    if (reference != nullptr) {
        if (ImGui::Button("Wind = reference")) {
            preview_wind_speed_ = reference->wind.reference_speed;
            apply_preview_wind();
        }
        ImGui::SetItemTooltip("Back to the reference wind of the selected contributor, "
                              "which is that contributor as authored.");
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

    ImGui::Spacing();
    ImGui::TextDisabled("Measures");
    ImGui::Text("Crest to trough: %.2f m", water::wave_height(water_) * 2.0f);
    ImGui::SetItemTooltip("With every crest of every contributor aligned, which "
                          "a sea only rarely arranges.");
    ImGui::Text("Longest train:   %.1f m", longest);
    // The period is what says whether a sea rolls. Wind chop comes in at two
    // to five seconds and reads as busy; swell that has travelled from a
    // storm arrives at eight to fourteen and reads as a procession of ridges.
    // The wavelength decides it, so this is a consequence rather than a
    // setting — but it is the number to steer by.
    const float omega = water::wave_frequency(longest) * longest_speed_scale;
    if (omega > 1e-4f) {
        const float period = glm::two_pi<float>() / omega;
        const char* reads = period < 6.0f ? "wind chop"
                          : (period < 8.0f ? "building" : "rolling swell");
        ImGui::Text("Longest period:  %.1f s", period);
        ImGui::TextDisabled("reads as %s", reads);
    }
    // The shortest train is what decides how finely the surface has to be
    // tessellated: under about four vertices per wavelength it cannot be
    // drawn and comes out as noise instead of chop.
    const float shortest = water::shortest_wavelength(water_);
    ImGui::Text("Shortest train:  %.1f m", shortest);
    ImGui::TextDisabled("needs ~%.1f m between vertices", shortest / 4.0f);
}

void WaterEditorContext::draw_wave_list_panel() {
    ImGui::Begin(ctx_title("Waves").c_str());
    if (!loaded_) {
        ImGui::TextDisabled("No .fjwater open.");
        ImGui::End();
        return;
    }

    GerstnerContributor* sea = selected_gerstner();
    if (sea == nullptr) {
        ImGui::TextDisabled("Trains belong to a Gerstner contributor.");
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("%s", contributor_label(*sea).c_str());
    ImGui::Text("%zu of %u trains", sea->waves.size(), MAX_WATER_WAVES);
    ImGui::SameLine();
    ImGui::BeginDisabled(sea->waves.size() >= MAX_WATER_WAVES);
    if (ImGui::SmallButton("Add")) {
        sea->waves.push_back(WaterWave{});
        push_live();
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    int remove = -1;
    for (std::size_t i = 0; i < sea->waves.size(); ++i) {
        auto& wave = sea->waves[i];
        ImGui::PushID(static_cast<int>(i));

        // Name each train by its size rather than its index: which one is
        // "the big swell" is the question being asked of the list.
        if (ImGui::CollapsingHeader(
                (std::to_string(i + 1) + ":  " + std::to_string(static_cast<int>(wave.wavelength))
                 + " m  /  " + std::to_string(wave.amplitude).substr(0, 4) + " m").c_str(),
                ImGuiTreeNodeFlags_DefaultOpen)) {
            float deg = direction_degrees(wave.direction);
            if (ImGui::SliderFloat("Direction", &deg, -180.0f, 180.0f, "%.0f deg")) {
                wave.direction = direction_from_degrees(deg);
                push_live();
            }
            if (ImGui::SliderFloat("Wavelength", &wave.wavelength, 0.5f, 200.0f, "%.1f m")) {
                push_live();
            }
            if (ImGui::SliderFloat("Amplitude", &wave.amplitude, 0.0f, 6.0f, "%.2f m")) {
                push_live();
            }
            if (ImGui::SliderFloat("Steepness", &wave.steepness, 0.0f, 1.0f, "%.2f")) {
                push_live();
            }
            if (ImGui::SliderFloat("Speed", &wave.speed, 0.0f, 3.0f, "%.2f")) {
                push_live();
            }

            // A train travels at the speed its wavelength implies, so the
            // period is a consequence rather than a setting — worth showing,
            // because two trains with close periods beat against each other.
            float omega = water::wave_frequency(wave.wavelength) * wave.speed
                        * sea->speed_scale;
            if (omega > 1e-4f) {
                ImGui::TextDisabled("period %.1f s", glm::two_pi<float>() / omega);
            }

            if (ImGui::SmallButton("Remove")) remove = static_cast<int>(i);
        }
        ImGui::PopID();
    }

    if (remove >= 0) {
        sea->waves.erase(sea->waves.begin() + remove);
        push_live();
    }

    ImGui::End();
}

} // namespace fjell
