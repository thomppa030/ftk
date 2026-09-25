#include "ui/project_settings_window.hpp"

#include "core/fixed_timestep.hpp"
#include "core/project_manager.hpp"
#include "core/project.hpp"
#include "renderer/render_settings.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/kit/pages.hpp"
#include "ui/kit/save_bar.hpp"
#include "ui/kit/search.hpp"
#include "ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <format>
#include <span>
#include <string>

#include <glm/ext/vector_int2.hpp>

namespace fjell {

namespace {

using Backend = RenderSettings::Ddgi::Backend;
using ReflectionMode = RenderSettings::Ssr::Mode;

inline constexpr ui::Choice<Backend> DDGI_BACKENDS[] = {
    {Backend::SdfFallback, "SDF"},
    {Backend::RayTraced, "Ray traced"},
};

// Ray traced last, so a GPU without ray tracing is offered the others.
inline constexpr ui::Choice<ReflectionMode> REFLECTION_MODES[] = {
    {ReflectionMode::Off, "Off"},
    {ReflectionMode::ScreenSpace, "Screen space"},
    {ReflectionMode::RayTraced, "Ray traced"},
};

// A value shown but not edited, on its own row.
void readout_row(const char* label, const std::string& value) {
    ui::row(label, [&] { ui::readout(value.c_str()); });
}

} // namespace

void ProjectSettingsWindow::init(VulkanContext* vk, ProjectManager* projects) {
    vk_ = vk;
    projects_ = projects;
}

namespace {

// A page as the list shows it.
struct PageInfo {
    const char* icon;
    const char* name;
};

} // namespace

void ProjectSettingsWindow::draw() {
    if (!visible_) { return; }

    static constexpr PageInfo PAGES[PAGE_COUNT] = {
        {ui::icon::rendering, "Rendering"},
        {ui::icon::ui_layout, "Game UI"},
        {ui::icon::world_environment, "Environment"},
        {ui::icon::simulation, "Simulation"},
    };

    Project* project = projects_ != nullptr ? projects_->active() : nullptr;
    std::string title = std::string(ui::icon::settings) + "  Project settings";
    if (project != nullptr) title += " \xc2\xb7 " + project->config().name;
    title += "###project_settings";

    auto window = ui::PagedWindow(title.c_str(), &visible_, {720.0f, 520.0f});
    if (!window) return;
    if (project == nullptr) {
        ui::empty_state(ui::icon::settings, "No project open", "Open or create one from the Project menu");
        return;
    }

    const float footer = ui::save_bar_height();
    const bool searching = !search_.empty();
    if (auto list = ui::PageList("##pages", footer)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ui::search_field("##search", search_);
        ImGui::Dummy({0.0f, theme::GAP_XS});
        for (std::size_t i = 0; i < PAGE_COUNT; ++i) {
            const auto page = static_cast<Page>(i);
            const bool dimmed = searching && found_[i] == 0;
            if (ui::page_entry(PAGES[i].icon, PAGES[i].name, !searching && page == page_, dimmed)) {
                page_ = page;
                search_.clear();
            }
        }
    }

    if (auto body = ui::PageBody("##page", footer)) {
        if (!searching) {
            ui::page_title(PAGES[static_cast<std::size_t>(page_)].icon, PAGES[static_cast<std::size_t>(page_)].name);
            draw_page(page_);
        } else {
            // Every page's matching settings, each under its title. A title
            // goes by what its page found the frame before: the rows only
            // count as they are drawn.
            int total = 0;
            for (std::size_t i = 0; i < PAGE_COUNT; ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (found_[i] > 0) ui::page_title(PAGES[i].icon, PAGES[i].name);
                const ui::RowFilter filter(search_);
                draw_page(static_cast<Page>(i));
                found_[i] = filter.matches();
                total += found_[i];
                ImGui::PopID();
            }
            if (total == 0) {
                ui::empty_state(ui::icon::search, "No settings match", "Try another word");
            }
        }
    }

    switch (ui::save_bar(unsaved(), "project.fjell")) {
        case ui::SaveAction::Save: on_save.broadcast(); break;
        case ui::SaveAction::Revert: revert(); break;
        case ui::SaveAction::None: break;
    }
}

void ProjectSettingsWindow::draw_page(Page page) {
    switch (page) {
        case Page::Rendering: draw_rendering(); break;
        case Page::GameUi: draw_game_ui(); break;
        case Page::Environment: draw_environment(); break;
        case Page::Simulation: draw_simulation(); break;
    }
}

bool ProjectSettingsWindow::unsaved() const {
    const Project* project = projects_->active();
    const ProjectConfig& now = project->config();
    const ProjectConfig& saved = project->saved_config();
    return now.render != saved.render || now.ui != saved.ui || now.environment != saved.environment ||
           now.simulation != saved.simulation;
}

void ProjectSettingsWindow::revert() {
    Project* project = projects_->active();
    ProjectConfig& config = project->config();
    const ProjectConfig& saved = project->saved_config();
    config.render = saved.render;
    config.ui = saved.ui;
    config.environment = saved.environment;
    config.simulation = saved.simulation;
    vk_->set_render_settings(config.render);
}

void ProjectSettingsWindow::draw_game_ui() {
    auto& ui_config = projects_->active()->config().ui;
    if (auto t = ui::PropertyTable("##game_ui")) {
        ui::row("Design resolution",
                "The frame game UI is designed in. The UI editor previews at this size and "
                "HUDs scale to keep these proportions at any viewport.",
                [&] {
                    glm::ivec2 size{static_cast<int>(ui_config.design_width),
                                    static_cast<int>(ui_config.design_height)};
                    if (ui::ivec2("##design", size, 1.0f, 240, 7680, ui::Unit::Pixels).changed) {
                        ui_config.design_width = static_cast<uint32_t>(std::max(size.x, 320));
                        ui_config.design_height = static_cast<uint32_t>(std::max(size.y, 240));
                    }
                });
        ui::row("Presets", [&] {
            struct Preset { const char* label; uint32_t width; uint32_t height; };
            static constexpr Preset PRESETS[] = {
                {"1920 \xc3\x97 1080", 1920, 1080},
                {"2560 \xc3\x97 1440", 2560, 1440},
                {"1280 \xc3\x97 720", 1280, 720},
            };
            bool first = true;
            for (const Preset& preset : PRESETS) {
                if (!first) ImGui::SameLine();
                first = false;
                if (ui::button(preset.label, ui::ButtonKind::Ghost)) {
                    ui_config.design_width = preset.width;
                    ui_config.design_height = preset.height;
                }
            }
        });
    }
}

void ProjectSettingsWindow::draw_environment() {
    auto& env = projects_->active()->config().environment;
    if (auto t = ui::PropertyTable("##environment")) {
        ui::row("Wetting", [&] {
            (void)ui::drag("##wetting", env.wetting_seconds,
                           {.speed = 1.0f, .min = 1.0f, .max = 3600.0f, .unit = ui::Unit::Seconds, .format = "%.0f"});
        });
        ui::row("Drying", [&] {
            (void)ui::drag("##drying", env.drying_seconds,
                           {.speed = 1.0f, .min = 1.0f, .max = 7200.0f, .unit = ui::Unit::Seconds, .format = "%.0f"});
        });
        ui::row("Snowing", [&] {
            (void)ui::drag("##snowing", env.snowing_seconds,
                           {.speed = 1.0f, .min = 1.0f, .max = 3600.0f, .unit = ui::Unit::Seconds, .format = "%.0f"});
        });
        ui::row("Melting", [&] {
            (void)ui::drag("##melting", env.melting_seconds,
                           {.speed = 1.0f, .min = 1.0f, .max = 7200.0f, .unit = ui::Unit::Seconds, .format = "%.0f"});
        });
        ui::hint("How long surfaces take to take on and shed the weather");
    }
}

void ProjectSettingsWindow::draw_simulation() {
    auto& simulation = projects_->active()->config().simulation;
    constexpr int MIN_RATE = static_cast<int>(FixedTimestep::MIN_TICK_RATE);
    constexpr int MAX_RATE = static_cast<int>(FixedTimestep::MAX_TICK_RATE);
    if (auto t = ui::PropertyTable("##simulation")) {
        ui::row("Fixed tick rate",
                "How many fixed steps the simulation takes per second of game time. Physics, "
                "scripts' fixed_tick and the environment's simulated half run at this rate, and "
                "drawing blends between their steps. Takes effect the next time play starts.",
                [&] {
                    int rate = static_cast<int>(simulation.fixed_tick_rate);
                    if (ui::drag_int("##fixed_tick_rate", rate, 0.5f, MIN_RATE, MAX_RATE, ui::Unit::Hertz)
                            .changed) {
                        simulation.fixed_tick_rate = static_cast<uint32_t>(std::clamp(rate, MIN_RATE, MAX_RATE));
                    }
                });
        const double slowest = static_cast<double>(simulation.fixed_tick_rate) /
                               FixedTimestep::MAX_STEPS_PER_FRAME;
        ui::hint(std::format("Below {:g} fps the game slows down: a frame runs at most {} steps",
                             slowest, FixedTimestep::MAX_STEPS_PER_FRAME)
                     .c_str());
    }
}

// Any edit in this section writes to the project's RenderSettings AND
// re-applies the whole blob through VulkanContext — cheap, keeps the
// live pipeline in sync with what the project will save.
void ProjectSettingsWindow::draw_rendering() {
    auto& s = projects_->active()->config().render;
    const bool rt_ok = vk_->ray_tracing_supported();
    ui::Edit edit;

    if (ui::subheading_foldable("Quality")) {
        if (auto t = ui::PropertyTable("##quality")) {
            // MSAA display-only: runtime changes would need swapchain + offscreen
            // recreation we don't support yet. Stored value applies on next launch.
            readout_row("MSAA", std::format("{}x", static_cast<uint32_t>(s.msaa_samples)));
            ui::hint("Fixed at launch");
        }
    }

    if (ui::subheading_foldable("Global illumination")) {
        if (auto t = ui::PropertyTable("##gi")) {
            ui::row("DDGI", [&] { edit |= ui::checkbox("##ddgi", s.ddgi.enabled); });
            if (s.ddgi.enabled) {
                ui::row("Backend", [&] {
                    if (rt_ok) {
                        edit |= ui::choice("##backend", s.ddgi.backend, DDGI_BACKENDS);
                    } else {
                        ui::readout("SDF");
                    }
                });
                if (!rt_ok) ui::hint("This GPU can't ray trace");
                ui::row("Probe count",
                        "Probes along each axis, cubed. The grid fits scene bounds, so more "
                        "probes means smaller cells. Cost is cubic, and changing it rebuilds "
                        "the probe atlases and discards accumulated lighting.",
                        [&] { edit |= ui::slider_int("##probes", s.ddgi.probe_count, 4, 48); });
                {
                    const auto& cfg = vk_->ddgi_pass().config();
                    ui::hint(std::format("{} probes, cell {:.2f} m",
                                         cfg.probe_counts.x * cfg.probe_counts.y * cfg.probe_counts.z,
                                         cfg.probe_spacing.x).c_str());
                }
                ui::row("GI intensity", [&] {
                    edit |= ui::slider("##gi_intensity", s.ddgi.gi_intensity, 0.0f, 2.0f);
                });
                ui::row("Infinite bounces", [&] {
                    edit |= ui::slider("##bounces", s.ddgi.infinite_bounces_mult, 0.0f, 3.0f,
                                       ui::Unit::Times);
                });
                ui::row("Normal bias",
                        "Lifts the sample toward the probe. Too low and a flat surface hits "
                        "the degenerate Chebyshev case (zero variance) and darkens at every probe.",
                        [&] { edit |= ui::slider("##normal_bias", s.ddgi.normal_bias, 0.0f, 1.0f); });
                ui::row("View bias", [&] {
                    edit |= ui::slider("##view_bias", s.ddgi.view_bias, 0.0f, 1.0f);
                });
                // Not part of RenderSettings yet — drives the pass directly
                // so this can be A/B'd without a project-format change.
                auto& dcfg = vk_->ddgi_pass().config();
                ui::row("Spatial filter",
                        "Blends each probe with its neighbours before sampling. Without it "
                        "the interpolation cage shows every probe-to-probe difference as a "
                        "visible lattice.",
                        [&] { (void)ui::checkbox("##spatial_filter", dcfg.spatial_filter); });
                if (dcfg.spatial_filter) {
                    ui::row("Filter angle", [&] {
                        (void)ui::slider("##filter_angle", dcfg.filter_max_hit_angle, 0.0f, 1.0f,
                                         ui::Unit::Radians);
                    });
                }
            }

            ui::row("SS-GI", [&] { edit |= ui::checkbox("##ssgi", s.ss_gi.enabled); });
            if (s.ss_gi.enabled) {
                ui::row("Intensity", [&] {
                    edit |= ui::slider("##ssgi_intensity", s.ss_gi.intensity, 0.0f, 10.0f);
                });
                ui::row("Trace distance", [&] {
                    edit |= ui::slider("##ssgi_trace", s.ss_gi.max_trace_dist, 1.0f, 100.0f,
                                       ui::Unit::Metres);
                });
                ui::row("Hysteresis", [&] {
                    edit |= ui::slider("##ssgi_hysteresis", s.ss_gi.hysteresis, 0.0f, 1.0f);
                });
            }
        }
    }

    if (ui::subheading_foldable("Post-processing")) {
        if (auto t = ui::PropertyTable("##post")) {
            ui::row("GTAO", [&] { edit |= ui::checkbox("##gtao", s.gtao.enabled); });
            ui::row("Bloom", [&] { edit |= ui::checkbox("##bloom", s.bloom.enabled); });
            // Skinned meshes don't appear in RT reflections in V1: the BLAS
            // holds bind-pose vertices only.
            ui::row("Reflections", [&] {
                const std::span<const ui::Choice<ReflectionMode>> modes(REFLECTION_MODES);
                edit |= ui::choice("##reflections", s.ssr.mode, rt_ok ? modes : modes.first(2));
            });
            if (!rt_ok) {
                ui::hint("This GPU can't ray trace");
            } else if (s.ssr.mode == ReflectionMode::RayTraced) {
                ui::hint("Skinned meshes reflect in their bind pose");
            }
            if (s.ssr.mode != ReflectionMode::Off) {
                ui::row("Intensity", [&] {
                    edit |= ui::slider("##ssr_intensity", s.ssr.intensity, 0.0f, 2.0f);
                });
                if (s.ssr.mode == ReflectionMode::ScreenSpace) {
                    ui::row("Thickness", [&] {
                        edit |= ui::slider("##ssr_thickness", s.ssr.thickness, 0.01f, 2.0f);
                    });
                }
            }
        }
    }

    // Temporary: one dial, exposed to find a value rather than because a
    // project has anything to say about it. See RenderSettings::SunShafts.
    if (ui::subheading_foldable("Sun shafts (temporary)")) {
        if (auto t = ui::PropertyTable("##shafts")) {
            ui::row("Depth tolerance",
                    "How far a half-res tap's surface may sit from a pixel's own, as a share "
                    "of that pixel's distance, before it stops counting in the upsample.\n\n"
                    "Lower keeps shaft light off the near side of a silhouette. Too low and "
                    "taps that agreed are rejected, leaving the upsample on its nearest-tap "
                    "fallback, which reads as blocky.\n\n"
                    "Temporary: here to find a value. 0.05 has held so far.",
                    [&] {
                        edit |= ui::slider("##shaft_tolerance", s.sun_shafts.upsample_depth_tolerance,
                                           0.01f, 0.5f);
                    });
        }
    }

    if (ui::subheading_foldable("Exposure")) {
        ui::hint("The tone curve and the grade are the post process volumes' shaders");
        if (auto t = ui::PropertyTable("##tonemap")) {
            ui::row("Auto exposure", [&] {
                edit |= ui::checkbox("##auto_exposure", s.tonemap.auto_exposure);
            });
            if (!s.tonemap.auto_exposure) {
                ui::row("Exposure", [&] {
                    edit |= ui::slider("##exposure", s.tonemap.exposure_ev, -5.0f, 5.0f,
                                       ui::Unit::ExposureValue, "%.1f");
                });
            } else {
                // Auto exposure drives the value; the panel only sets how
                // fast it adapts, and reads the result back.
                readout_row("Exposure", std::format("{:.2f} EV", vk_->post_pass().exposure()));
                ui::row("Adaptation speed", [&] {
                    edit |= ui::slider("##adaptation", s.tonemap.adaptation_speed, 0.1f, 5.0f);
                });
            }
            // Runtime only, like the debug light channel: a way of looking,
            // not a project setting, so it is never saved.
            ui::row("Ungraded",
                    "Draw the engine's default look, whatever post process volumes the camera "
                    "is in: the frame as it is before a grade, to capture and grade elsewhere. "
                    "Not saved.",
                    [&] {
                        bool ungraded = vk_->post_pass().ungraded();
                        if (ui::checkbox("##ungraded", ungraded).changed) {
                            vk_->post_pass().set_ungraded(ungraded);
                        }
                    });
        }
    }

    if (ui::subheading_foldable("Frame graph", false)) {
        if (auto t = ui::PropertyTable("##frame_graph")) {
            ui::row("Aliasing",
                    "When off, each transient image gets its own VkImage. Use to isolate "
                    "regressions caused by aliasing reuse.",
                    [&] { edit |= ui::checkbox("##aliasing", s.frame_graph_aliasing); });
            ui::row("Async compute",
                    "Record compute passes on the compute queue so they overlap the "
                    "graphics work. Costs three submits a frame; only pays off when "
                    "there is enough compute to hide.",
                    [&] { edit |= ui::checkbox("##async_compute", s.async_compute); });
            if (!vk_->async_compute_supported()) ui::hint("This GPU has no compute queue");

            const auto& st = vk_->render_pipeline().aliasing_stats();
            const double ratio = st.logical_count == 0 ? 0.0
                : 100.0 * (1.0 - (static_cast<double>(st.physical_count)
                                   / static_cast<double>(st.logical_count)));
            readout_row("Logical -> physical",
                        std::format("{} -> {} ({:.1f}%)", st.logical_count, st.physical_count, ratio));
            readout_row("Pool", std::format("{} live, {:.1f} MiB", st.live_images,
                                            static_cast<double>(st.live_bytes) / (1024.0 * 1024.0)));
            readout_row("Allocations", std::format("{} acquires, {} new", st.acquires, st.allocations));
        }
    }

    if (edit.changed) {
        vk_->set_render_settings(s);
    }
}

} // namespace fjell
