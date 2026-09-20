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
#include "renderer/resources/water_surface.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/dockspace_layout.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
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

} // namespace

WaterEditorContext::WaterEditorContext(WaterEditorDeps deps) : deps_{deps} {}

void WaterEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Water Preview");
    init_shared_panels(deps_.vulkan_context, deps_.project_mgr);
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
    water->effective_water = water::apply_wind(water_, wind);
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

void WaterEditorContext::push_live() {
    dirty_ = true;

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
        water->effective_water = water_;
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
    ictx.file_browser = deps_.file_browser;
    ictx.icons = deps_.icon_cache;

    static const std::string mat_exts[] = {".fjmat"};
    const auto* mat_type = AssetTypeRegistry::instance().find_by_name("Material");
    if (asset_ref_field({.label = "Material",
                         .id = "##WaterPreviewMat",
                         .extensions = mat_exts,
                         .ctx = &ictx,
                         .asset_type = mat_type},
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

    GerstnerContributor* sea = selected_gerstner();
    if (sea == nullptr) {
        ImGui::TextDisabled("No contributor selected.");
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
    ImGui::TextDisabled("Whole sea");
    if (ImGui::SliderFloat("Amplitude scale", &sea->amplitude_scale, 0.0f, 4.0f, "%.2f")) {
        push_live();
    }
    ImGui::SetItemTooltip("Multiplies every train. Where calm becomes storm "
                          "without touching a single wave.");
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

    // What that setting costs, measured rather than asserted: sample the
    // surface and count where it has folded. A number beats a warning here,
    // because a little folding at a distance may be a trade worth making.
    if (!sea->waves.empty()) {
        int folded = 0;
        int total = 0;
        for (float x = -120.0f; x <= 120.0f; x += 6.0f) {
            for (float z = -120.0f; z <= 120.0f; z += 6.0f) {
                for (float t : {0.0f, 5.5f, 13.7f}) {
                    ++total;
                    if (water::crest(water_, {x, z}, t) >= 0.999f) ++folded;
                }
            }
        }
        float pct = 100.0f * static_cast<float>(folded) / static_cast<float>(total);
        if (pct <= 0.0f) {
            ImGui::TextDisabled("no folding");
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                               "%.2f%% of the surface folds", pct);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Wind response");
    if (ImGui::Checkbox("Answers the weather", &sea->wind.enabled)) {
        push_live();
    }
    ImGui::SetItemTooltip("Whether the weather's wind reaches this water. Off "
                          "for a pool, a canal, anything indoors.");
    if (sea->wind.enabled) {
        // The wind blowing on the preview, which is the editor's and not the
        // asset's: a .fjwater says how a sea answers wind, never what wind it
        // sits in. Without it the four responses below are dials with nothing
        // to answer them, since no weather blows through the preview scene.
        if (ImGui::SliderFloat("Preview wind", &preview_wind_speed_,
                               0.0f, 40.0f, "%.1f m/s")) {
            apply_preview_wind();
        }
        ImGui::SetItemTooltip("What the preview is blown at, to watch the "
                              "responses below. Not saved — a placed sea takes "
                              "its wind from the weather.");
        if (ImGui::SliderFloat("Preview wind heading", &preview_wind_heading_,
                               -180.0f, 180.0f, "%.0f deg")) {
            apply_preview_wind();
        }
        if (ImGui::Button("Wind = reference")) {
            preview_wind_speed_ = sea->wind.reference_speed;
            apply_preview_wind();
        }
        ImGui::SetItemTooltip("Back to the sea as authored.");
        ImGui::Spacing();

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

                const float reference = std::max(sea->wind.reference_speed, 0.1f);
                for (float factor : {0.0f, 0.5f, 1.0f, 1.5f, 2.0f}) {
                    const float speed = reference * factor;
                    // Height and headroom are this contributor's own answer;
                    // whether the surface folds is a question about the whole
                    // sea under that wind, every contributor answering it.
                    const GerstnerContributor blown_sea =
                        water::apply_wind(*sea, {speed, 0.0f});
                    const FjWaterData blown = water::apply_wind(water_, {speed, 0.0f});

                    int folded = 0;
                    int total = 0;
                    for (float x = -120.0f; x <= 120.0f; x += 12.0f) {
                        for (float z = -120.0f; z <= 120.0f; z += 12.0f) {
                            for (float t : {0.0f, 7.3f}) {
                                ++total;
                                if (water::crest(blown, {x, z}, t) >= 0.999f) ++folded;
                            }
                        }
                    }
                    const float pct = 100.0f * static_cast<float>(folded)
                                    / static_cast<float>(total);

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

    ImGui::Separator();
    ImGui::TextDisabled("Measures");
    ImGui::Text("Crest to trough: %.2f m", water::wave_height(water_) * 2.0f);
    if (!sea->waves.empty()) {
        float longest = 0.0f;
        for (const auto& w : sea->waves) longest = std::max(longest, w.wavelength);
        ImGui::Text("Longest train:   %.1f m", longest);
        // The period is what says whether a sea rolls. Wind chop comes in at
        // two to five seconds and reads as busy; swell that has travelled from
        // a storm arrives at eight to fourteen and reads as a procession of
        // ridges. The wavelength decides it, so this is a consequence rather
        // than a setting — but it is the number to steer by.
        {
            float omega = water::wave_frequency(longest) * sea->speed_scale;
            if (omega > 1e-4f) {
                float period = glm::two_pi<float>() / omega;
                const char* reads = period < 6.0f ? "wind chop"
                                  : (period < 8.0f ? "building" : "rolling swell");
                ImGui::Text("Longest period:  %.1f s", period);
                ImGui::TextDisabled("reads as %s", reads);
            }
        }
        // The shortest train is what decides how finely the surface has to be
        // tessellated: under about four vertices per wavelength it cannot be
        // drawn and comes out as noise instead of chop.
        float shortest = sea->waves.front().wavelength;
        for (const auto& w : sea->waves) shortest = std::min(shortest, w.wavelength);
        ImGui::Text("Shortest train:  %.1f m", shortest);
        ImGui::TextDisabled("needs ~%.1f m between vertices", shortest / 4.0f);
    }

    ImGui::End();
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
        ImGui::TextDisabled("No contributor selected.");
        ImGui::End();
        return;
    }

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
