#include "ui/water_editor_context.hpp"

#include "core/log.hpp"
#include "core/project.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/fjwater_io.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/resources/water_surface.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/dockspace_layout.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

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
    // The scene's own sky, not the studio backdrop: a sea is lit by the sky
    // over it and reflects it, so the two are judged together.
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

float WaterEditorContext::total_amplitude() const {
    float sum = 0.0f;
    for (const auto& wave : water_.waves) sum += wave.amplitude;
    return sum * water_.amplitude_scale;
}

void WaterEditorContext::push_live() {
    dirty_ = true;

    // A water node holds its own copy of the field it was loaded with, and
    // the pass draws from that copy — so a sea does not change because the
    // file did. Hand every node that names this asset the edited field, and
    // the viewport shows the sea being shaped rather than the one last
    // saved.
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
    }
}

void WaterEditorContext::draw(float dt) {
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

    ImGui::TextDisabled("Generate");
    ImGui::TextWrapped(
        "Fills the wave list from a few numbers. Overwrites whatever is "
        "there — it is a starting shape, and the list below is the asset.");
    ImGui::Spacing();

    auto& g = water_.generator;
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
        water_.waves = generate_water_waves(g);
        push_live();
    }
    // Moving a generator slider regenerates as it is dragged. The generator
    // is deterministic, so this is the same list the button would make, and
    // shaping a sea by watching it beats shaping it by pressing a button.
    if (changed) {
        water_.waves = generate_water_waves(g);
        push_live();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Whole sea");
    if (ImGui::SliderFloat("Amplitude scale", &water_.amplitude_scale, 0.0f, 4.0f, "%.2f")) {
        push_live();
    }
    ImGui::SetItemTooltip("Multiplies every train. Where calm becomes storm "
                          "without touching a single wave.");
    if (ImGui::SliderFloat("Speed scale", &water_.speed_scale, 0.0f, 3.0f, "%.2f")) {
        push_live();
    }
    if (ImGui::SliderFloat("Steepness headroom", &water_.steepness_headroom,
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
    if (!water_.waves.empty()) {
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
    ImGui::TextDisabled("Measures");
    ImGui::Text("Crest to trough: %.2f m", total_amplitude() * 2.0f);
    if (!water_.waves.empty()) {
        float longest = 0.0f;
        for (const auto& w : water_.waves) longest = std::max(longest, w.wavelength);
        ImGui::Text("Longest train:   %.1f m", longest);
        // The period is what says whether a sea rolls. Wind chop comes in at
        // two to five seconds and reads as busy; swell that has travelled from
        // a storm arrives at eight to fourteen and reads as a procession of
        // ridges. The wavelength decides it, so this is a consequence rather
        // than a setting — but it is the number to steer by.
        {
            float omega = water::wave_frequency(longest) * water_.speed_scale;
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
        float shortest = water_.waves.front().wavelength;
        for (const auto& w : water_.waves) shortest = std::min(shortest, w.wavelength);
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

    ImGui::Text("%zu of %u trains", water_.waves.size(), MAX_WATER_WAVES);
    ImGui::SameLine();
    ImGui::BeginDisabled(water_.waves.size() >= MAX_WATER_WAVES);
    if (ImGui::SmallButton("Add")) {
        water_.waves.push_back(WaterWave{});
        push_live();
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    int remove = -1;
    for (std::size_t i = 0; i < water_.waves.size(); ++i) {
        auto& wave = water_.waves[i];
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
                        * water_.speed_scale;
            if (omega > 1e-4f) {
                ImGui::TextDisabled("period %.1f s", glm::two_pi<float>() / omega);
            }

            if (ImGui::SmallButton("Remove")) remove = static_cast<int>(i);
        }
        ImGui::PopID();
    }

    if (remove >= 0) {
        water_.waves.erase(water_.waves.begin() + remove);
        push_live();
    }

    ImGui::End();
}

} // namespace fjell
