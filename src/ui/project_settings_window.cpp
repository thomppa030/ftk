#include "ui/project_settings_window.hpp"

#include "core/fixed_timestep.hpp"
#include "core/project_manager.hpp"
#include "core/project.hpp"
#include "renderer/render_settings.hpp"
#include "renderer/vulkan_context.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ui/icons.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/pages.hpp"
#include "ftk/ui/kit/save_bar.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/theme.hpp"

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

inline constexpr ftk::ui::Choice<Backend> DDGI_BACKENDS[] = {
    {Backend::SdfFallback, "SDF"},
    {Backend::RayTraced, "Ray traced"},
};

// Ray traced last, so a GPU without ray tracing is offered the others.
inline constexpr ftk::ui::Choice<ReflectionMode> REFLECTION_MODES[] = {
    {ReflectionMode::Off, "Off"},
    {ReflectionMode::ScreenSpace, "Screen space"},
    {ReflectionMode::RayTraced, "Ray traced"},
};

// A value shown but not edited, on its own row.
void readout_row(const char* label, const std::string& value) {
    ftk::ui::row(label, [&] { ftk::ui::readout(value.c_str()); });
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
        {icon::rendering, "Rendering"},
        {icon::ui_layout, "Game UI"},
        {icon::world_environment, "Environment"},
        {icon::simulation, "Simulation"},
    };

    Project* project = projects_ != nullptr ? projects_->active() : nullptr;
    std::string title = std::string(icon::settings) + "  Project settings";
    if (project != nullptr) title += " \xc2\xb7 " + project->config().name;
    title += "###project_settings";

    auto window = ftk::ui::PagedWindow(title.c_str(), &visible_, {720.0f, 520.0f});
    if (!window) return;
    if (project == nullptr) {
        ftk::ui::empty_state(icon::settings, "No project open", "Open or create one from the Project menu");
        return;
    }

    const float footer = ftk::ui::save_bar_height();
    const bool searching = !search_.empty();
    if (auto list = ftk::ui::PageList("##pages", footer)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ftk::ui::search_field("##search", search_);
        ImGui::Dummy({0.0f, ftk::theme::GAP_XS});
        for (std::size_t i = 0; i < PAGE_COUNT; ++i) {
            const auto page = static_cast<Page>(i);
            const bool dimmed = searching && found_[i] == 0;
            if (ftk::ui::page_entry(PAGES[i].icon, PAGES[i].name, !searching && page == page_, dimmed)) {
                page_ = page;
                search_.clear();
            }
        }
    }

    if (auto body = ftk::ui::PageBody("##page", footer)) {
        if (!searching) {
            ftk::ui::page_title(PAGES[static_cast<std::size_t>(page_)].icon, PAGES[static_cast<std::size_t>(page_)].name);
            draw_page(page_);
        } else {
            // Every page's matching settings, each under its title. A title
            // goes by what its page found the frame before: the rows only
            // count as they are drawn.
            int total = 0;
            for (std::size_t i = 0; i < PAGE_COUNT; ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (found_[i] > 0) ftk::ui::page_title(PAGES[i].icon, PAGES[i].name);
                const ftk::ui::RowFilter filter(search_);
                draw_page(static_cast<Page>(i));
                found_[i] = filter.matches();
                total += found_[i];
                ImGui::PopID();
            }
            if (total == 0) {
                ftk::ui::empty_state(icon::search, "No settings match", "Try another word");
            }
        }
    }

    switch (ftk::ui::save_bar(unsaved(), "project.fjell")) {
        case ftk::ui::SaveAction::Save: on_save.broadcast(); break;
        case ftk::ui::SaveAction::Revert: revert(); break;
        case ftk::ui::SaveAction::None: break;
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
    if (auto t = ftk::ui::PropertyTable("##game_ui")) {
        ftk::ui::row("Design resolution",
                "The frame game UI is designed in. The UI editor previews at this size and "
                "HUDs scale to keep these proportions at any viewport.",
                [&] {
                    glm::ivec2 size{static_cast<int>(ui_config.design_width),
                                    static_cast<int>(ui_config.design_height)};
                    if (ftk::ui::ivec2("##design", size, 1.0f, 240, 7680, ftk::ui::Unit::Pixels).changed) {
                        ui_config.design_width = static_cast<uint32_t>(std::max(size.x, 320));
                        ui_config.design_height = static_cast<uint32_t>(std::max(size.y, 240));
                    }
                });
        ftk::ui::row("Presets", [&] {
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
                if (ftk::ui::button(preset.label, ftk::ui::ButtonKind::Ghost)) {
                    ui_config.design_width = preset.width;
                    ui_config.design_height = preset.height;
                }
            }
        });
    }
}

void ProjectSettingsWindow::draw_environment() {
    auto& env = projects_->active()->config().environment;
    if (auto t = ftk::ui::PropertyTable("##environment")) {
        ftk::ui::row("Wetting", [&] {
            (void)ftk::ui::drag("##wetting", env.wetting_seconds,
                           {.speed = 1.0f, .min = 1.0f, .max = 3600.0f, .unit = ftk::ui::Unit::Seconds, .format = "%.0f"});
        });
        ftk::ui::row("Drying", [&] {
            (void)ftk::ui::drag("##drying", env.drying_seconds,
                           {.speed = 1.0f, .min = 1.0f, .max = 7200.0f, .unit = ftk::ui::Unit::Seconds, .format = "%.0f"});
        });
        ftk::ui::row("Snowing", [&] {
            (void)ftk::ui::drag("##snowing", env.snowing_seconds,
                           {.speed = 1.0f, .min = 1.0f, .max = 3600.0f, .unit = ftk::ui::Unit::Seconds, .format = "%.0f"});
        });
        ftk::ui::row("Melting", [&] {
            (void)ftk::ui::drag("##melting", env.melting_seconds,
                           {.speed = 1.0f, .min = 1.0f, .max = 7200.0f, .unit = ftk::ui::Unit::Seconds, .format = "%.0f"});
        });
        ftk::ui::hint("How long surfaces take to take on and shed the weather");
    }
}

void ProjectSettingsWindow::draw_simulation() {
    auto& simulation = projects_->active()->config().simulation;
    constexpr int MIN_RATE = static_cast<int>(FixedTimestep::MIN_TICK_RATE);
    constexpr int MAX_RATE = static_cast<int>(FixedTimestep::MAX_TICK_RATE);
    if (auto t = ftk::ui::PropertyTable("##simulation")) {
        ftk::ui::row("Fixed tick rate",
                "How many fixed steps the simulation takes per second of game time. Physics, "
                "scripts' fixed_tick and the environment's simulated half run at this rate, and "
                "drawing blends between their steps. Takes effect the next time play starts.",
                [&] {
                    int rate = static_cast<int>(simulation.fixed_tick_rate);
                    if (ftk::ui::drag_int("##fixed_tick_rate", rate, 0.5f, MIN_RATE, MAX_RATE, ftk::ui::Unit::Hertz)
                            .changed) {
                        simulation.fixed_tick_rate = static_cast<uint32_t>(std::clamp(rate, MIN_RATE, MAX_RATE));
                    }
                });
        const double slowest = static_cast<double>(simulation.fixed_tick_rate) /
                               FixedTimestep::MAX_STEPS_PER_FRAME;
        ftk::ui::hint(std::format("Below {:g} fps the game slows down: a frame runs at most {} steps",
                             slowest, FixedTimestep::MAX_STEPS_PER_FRAME)
                     .c_str());
        ftk::ui::row("Gone nodes remembered",
                "How many of the nodes that leave the scene during play keep their names, the last "
                "ones to go, so that a script using a Ref whose node is gone is told that node's "
                "name. Takes effect the next time play starts.",
                [&] {
                    constexpr int MAX = static_cast<int>(ProjectConfig::SimulationConfig::MAX_GONE_NODES_REMEMBERED);
                    int count = static_cast<int>(simulation.gone_nodes_remembered);
                    if (ftk::ui::drag_int("##gone_nodes_remembered", count, 4.0f, 0, MAX).changed) {
                        simulation.gone_nodes_remembered = static_cast<uint32_t>(std::clamp(count, 0, MAX));
                    }
                });
    }
}

// Any edit in this section writes to the project's RenderSettings AND
// re-applies the whole blob through VulkanContext — cheap, keeps the
// live pipeline in sync with what the project will save.
void ProjectSettingsWindow::draw_rendering() {
    auto& s = projects_->active()->config().render;
    const bool rt_ok = vk_->ray_tracing_supported();
    ftk::ui::Edit edit;

    if (ftk::ui::subheading_foldable("Quality")) {
        if (auto t = ftk::ui::PropertyTable("##quality")) {
            // MSAA display-only: runtime changes would need swapchain + offscreen
            // recreation we don't support yet. Stored value applies on next launch.
            readout_row("MSAA", std::format("{}x", static_cast<uint32_t>(s.msaa_samples)));
            ftk::ui::hint("Fixed at launch");
        }
    }

    if (ftk::ui::subheading_foldable("Global illumination")) {
        if (auto t = ftk::ui::PropertyTable("##gi")) {
            ftk::ui::row("DDGI", [&] { edit |= ftk::ui::checkbox("##ddgi", s.ddgi.enabled); });
            if (s.ddgi.enabled) {
                ftk::ui::row("Backend", [&] {
                    if (rt_ok) {
                        edit |= ftk::ui::choice("##backend", s.ddgi.backend, DDGI_BACKENDS);
                    } else {
                        ftk::ui::readout("SDF");
                    }
                });
                if (!rt_ok) ftk::ui::hint("This GPU can't ray trace");
                ftk::ui::row("Probe count",
                        "Probes along each axis, cubed. The grid fits scene bounds, so more "
                        "probes means smaller cells. Cost is cubic, and changing it rebuilds "
                        "the probe atlases and discards accumulated lighting.",
                        [&] { edit |= ftk::ui::slider_int("##probes", s.ddgi.probe_count, 4, 48); });
                {
                    const auto& cfg = vk_->ddgi_pass().config();
                    ftk::ui::hint(std::format("{} probes, cell {:.2f} m",
                                         cfg.probe_counts.x * cfg.probe_counts.y * cfg.probe_counts.z,
                                         cfg.probe_spacing.x).c_str());
                }
                ftk::ui::row("Bounce intensity", [&] {
                    edit |= ftk::ui::slider("##gi_intensity", s.ddgi.gi_intensity, 0.0f, 2.0f);
                });
                ftk::ui::row("Infinite bounces", [&] {
                    edit |= ftk::ui::slider("##bounces", s.ddgi.infinite_bounces_mult, 0.0f, 3.0f,
                                       ftk::ui::Unit::Times);
                });
                ftk::ui::row("Normal bias",
                        "Lifts the sample toward the probe. Too low and a flat surface hits "
                        "the degenerate Chebyshev case (zero variance) and darkens at every probe.",
                        [&] { edit |= ftk::ui::slider("##normal_bias", s.ddgi.normal_bias, 0.0f, 1.0f); });
                ftk::ui::row("View bias", [&] {
                    edit |= ftk::ui::slider("##view_bias", s.ddgi.view_bias, 0.0f, 1.0f);
                });
            }

            ftk::ui::row("SS-GI", [&] { edit |= ftk::ui::checkbox("##ssgi", s.ss_gi.enabled); });
            if (s.ss_gi.enabled) {
                ftk::ui::row("Intensity", [&] {
                    edit |= ftk::ui::slider("##ssgi_intensity", s.ss_gi.intensity, 0.0f, 10.0f);
                });
                ftk::ui::row("Trace distance", [&] {
                    edit |= ftk::ui::slider("##ssgi_trace", s.ss_gi.max_trace_dist, 1.0f, 100.0f,
                                       ftk::ui::Unit::Metres);
                });
                ftk::ui::row("Hysteresis", [&] {
                    edit |= ftk::ui::slider("##ssgi_hysteresis", s.ss_gi.hysteresis, 0.0f, 1.0f);
                });
            }
        }
    }

    if (ftk::ui::subheading_foldable("Post-processing")) {
        if (auto t = ftk::ui::PropertyTable("##post")) {
            ftk::ui::row("GTAO", [&] { edit |= ftk::ui::checkbox("##gtao", s.gtao.enabled); });
            ftk::ui::row("Bloom", [&] { edit |= ftk::ui::checkbox("##bloom", s.bloom.enabled); });
            // Skinned meshes don't appear in RT reflections in V1: the BLAS
            // holds bind-pose vertices only.
            ftk::ui::row("Reflections", [&] {
                const std::span<const ftk::ui::Choice<ReflectionMode>> modes(REFLECTION_MODES);
                edit |= ftk::ui::choice("##reflections", s.ssr.mode, rt_ok ? modes : modes.first(2));
            });
            if (!rt_ok) {
                ftk::ui::hint("This GPU can't ray trace");
            } else if (s.ssr.mode == ReflectionMode::RayTraced) {
                ftk::ui::hint("Skinned meshes reflect in their bind pose");
            }
            if (s.ssr.mode != ReflectionMode::Off) {
                ftk::ui::row("Intensity", [&] {
                    edit |= ftk::ui::slider("##ssr_intensity", s.ssr.intensity, 0.0f, 2.0f);
                });
                if (s.ssr.mode == ReflectionMode::ScreenSpace) {
                    ftk::ui::row("Thickness", [&] {
                        edit |= ftk::ui::slider("##ssr_thickness", s.ssr.thickness, 0.01f, 2.0f);
                    });
                }
            }
        }
    }

    // Temporary: one dial, exposed to find a value rather than because a
    // project has anything to say about it. See RenderSettings::SunShafts.
    if (ftk::ui::subheading_foldable("Sun shafts (temporary)")) {
        if (auto t = ftk::ui::PropertyTable("##shafts")) {
            ftk::ui::row("Depth tolerance",
                    "How far a half-res tap's surface may sit from a pixel's own, as a share "
                    "of that pixel's distance, before it stops counting in the upsample.\n\n"
                    "Lower keeps shaft light off the near side of a silhouette. Too low and "
                    "taps that agreed are rejected, leaving the upsample on its nearest-tap "
                    "fallback, which reads as blocky.\n\n"
                    "Temporary: here to find a value. 0.05 has held so far.",
                    [&] {
                        edit |= ftk::ui::slider("##shaft_tolerance", s.sun_shafts.upsample_depth_tolerance,
                                           0.01f, 0.5f);
                    });
        }
    }

    if (ftk::ui::subheading_foldable("Exposure")) {
        ftk::ui::hint("The tone curve and the grade are the post process volumes' shaders");
        if (auto t = ftk::ui::PropertyTable("##tonemap")) {
            ftk::ui::row("Auto exposure", [&] {
                edit |= ftk::ui::checkbox("##auto_exposure", s.tonemap.auto_exposure);
            });
            if (!s.tonemap.auto_exposure) {
                ftk::ui::row("Exposure", [&] {
                    edit |= ftk::ui::slider("##exposure", s.tonemap.exposure_ev, -5.0f, 5.0f,
                                       ftk::ui::Unit::ExposureValue, "%.1f");
                });
            } else {
                // Auto exposure drives the value; the panel only sets how
                // fast it adapts, and reads the result back.
                readout_row("Exposure", std::format("{:.2f} EV", vk_->post_pass().exposure()));
                ftk::ui::row("Adaptation speed", [&] {
                    edit |= ftk::ui::slider("##adaptation", s.tonemap.adaptation_speed, 0.1f, 5.0f);
                });
            }
            // Runtime only, like the debug light channel: a way of looking,
            // not a project setting, so it is never saved.
            ftk::ui::row("Ungraded",
                    "Draw the engine's default look, whatever post process volumes the camera "
                    "is in: the frame as it is before a grade, to capture and grade elsewhere. "
                    "Not saved.",
                    [&] {
                        bool ungraded = vk_->post_pass().ungraded();
                        if (ftk::ui::checkbox("##ungraded", ungraded).changed) {
                            vk_->post_pass().set_ungraded(ungraded);
                        }
                    });
        }
    }

    if (ftk::ui::subheading_foldable("Frame graph", false)) {
        if (auto t = ftk::ui::PropertyTable("##frame_graph")) {
            ftk::ui::row("Aliasing",
                    "When off, each transient image gets memory of its own. Use to isolate "
                    "regressions caused by aliasing reuse.",
                    [&] { edit |= ftk::ui::checkbox("##aliasing", s.frame_graph_aliasing); });
            ftk::ui::row("Async compute",
                    "Record compute passes on the compute queue so they overlap the "
                    "graphics work. Costs three submits a frame; only pays off when "
                    "there is enough compute to hide.",
                    [&] { edit |= ftk::ui::checkbox("##async_compute", s.async_compute); });
            if (!vk_->async_compute_supported()) ftk::ui::hint("This GPU has no compute queue");

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
