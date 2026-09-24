#include "ui/inspectors/vfx_module_inspectors.hpp"

#include "renderer/systems/vfx_module.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/imgui_curve_editor.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/color_field.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"

#include <glm/glm.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace fjell {

namespace {

inline constexpr ui::Choice<SpawnShape> SHAPES[] = {
    {SpawnShape::point, "Point"}, {SpawnShape::sphere, "Sphere"}, {SpawnShape::box, "Box"},
    {SpawnShape::cone, "Cone"},   {SpawnShape::ring, "Ring"},     {SpawnShape::edge, "Edge"},
    {SpawnShape::disk, "Disk"},
};
inline constexpr ui::Choice<VelocityMode> VELOCITY_MODES[] = {
    {VelocityMode::directional, "Directional"}, {VelocityMode::random, "Random"}, {VelocityMode::cone, "Cone"},
};
inline constexpr ui::Choice<OrbitalMode> ORBITAL_MODES[] = {
    {OrbitalMode::vortex, "Vortex"}, {OrbitalMode::orbit, "Orbit"}, {OrbitalMode::swirl, "Swirl"},
};
inline constexpr ui::Choice<CollisionModule::Source> COLLISION_SOURCES[] = {
    {CollisionModule::Source::sdf, "Static geometry"},
    {CollisionModule::Source::depth, "Depth buffer"},
    {CollisionModule::Source::both, "Both"},
    {CollisionModule::Source::overhead, "Shelter above"},
};
inline constexpr ui::Choice<CollisionModule::Response> COLLISION_RESPONSES[] = {
    {CollisionModule::Response::bounce, "Bounce"},
    {CollisionModule::Response::slide, "Slide"},
    {CollisionModule::Response::kill, "Kill"},
};
inline constexpr ui::Choice<FlipbookModule::Mode> FLIPBOOK_MODES[] = {
    {FlipbookModule::Mode::over_lifetime, "Over lifetime"},
    {FlipbookModule::Mode::fps_loop, "At a rate, looping"},
    {FlipbookModule::Mode::fps_clamp, "At a rate, once"},
};
inline constexpr ui::Choice<ParticleMeshShape> MESH_SHAPES[] = {
    {ParticleMeshShape::cube, "Cube"}, {ParticleMeshShape::sphere, "Sphere"}, {ParticleMeshShape::custom, "Custom"},
};
inline constexpr ui::Choice<SubEmitterTrigger> TRIGGERS[] = {
    {SubEmitterTrigger::death, "Death"}, {SubEmitterTrigger::collision, "Collision"}, {SubEmitterTrigger::both, "Both"},
};

// A range of two numbers as its own two rows, the second never under the first.
ui::Edit min_max(float& lo, float& hi, const ui::DragSpec& spec) {
    ui::Edit edit;
    ui::row("Min", [&] { edit |= ui::drag("##min", lo, spec); });
    ui::row("Max", [&] { edit |= ui::drag("##max", hi, spec); });
    if (hi < lo) hi = lo;
    return edit;
}

// An angle stored in radians and edited in degrees.
ui::Edit degrees(const char* id, float& radians, const ui::DragSpec& spec) {
    float deg = glm::degrees(radians);
    const ui::Edit edit = ui::drag(id, deg, spec);
    if (edit.changed) radians = glm::radians(deg);
    return edit;
}

// A curve with ready-made shapes under it.
template <typename... Presets>
ui::Edit curve_with_presets(const char* id, Curve& curve, Presets&&... presets) {
    ui::Edit edit;
    if (CurveEditor(id, curve, {-1, 120}, 0.0f, 1.0f)) edit = {true, true};
    auto apply = [&](const char* label, Curve shape) {
        if (ui::button(label, ui::ButtonKind::Ghost)) {
            curve = shape;
            edit = {true, true};
        }
        ImGui::SameLine();
    };
    (apply(presets.first, presets.second), ...);
    ImGui::NewLine();
    return edit;
}

} // namespace

ui::Edit draw_vfx_module_fields(VfxModule& module, const InspectorContext& ctx) {
    ui::Edit edit;
    using U = ui::Unit;
    if (auto t = ui::PropertyTable("##module")) {
        switch (module.type()) {
            case ModuleType::spawn_rate: {
                auto& m = static_cast<SpawnRateModule&>(module);
                ui::row("Rate", "Particles a second.", [&] {
                    edit |= ui::drag("##rate", m.rate, {.speed = 1.0f, .min = 0.0f, .max = 10000.0f, .format = "%.0f"});
                });
                break;
            }
            case ModuleType::initial_lifetime: {
                auto& m = static_cast<InitialLifetimeModule&>(module);
                ui::subheading("Lifetime");
                edit |= min_max(m.lifetime_min, m.lifetime_max, {.speed = 0.01f, .min = 0.01f, .max = 60.0f, .unit = U::Seconds});
                break;
            }
            case ModuleType::initial_position: {
                auto& m = static_cast<InitialPositionModule&>(module);
                ui::row("Shape", [&] { edit |= ui::choice("##shape", m.shape, SHAPES); });
                if (m.shape == SpawnShape::sphere || m.shape == SpawnShape::cone || m.shape == SpawnShape::ring ||
                    m.shape == SpawnShape::disk) {
                    ui::row("Radius", [&] {
                        edit |= ui::drag("##radius", m.radius, {.speed = 0.01f, .min = 0.0f, .max = 100.0f, .unit = U::Metres});
                    });
                }
                if (m.shape == SpawnShape::box) {
                    ui::row("Half extents", [&] {
                        edit |= ui::vec3("##extents", m.half_extents, {.speed = 0.01f, .min = 0.0f, .max = 100.0f, .unit = U::Metres});
                    });
                }
                if (m.shape == SpawnShape::edge) {
                    ui::row("Length", [&] {
                        edit |= ui::drag("##length", m.edge_length, {.speed = 0.01f, .min = 0.0f, .max = 100.0f, .unit = U::Metres});
                    });
                }
                break;
            }
            case ModuleType::initial_velocity: {
                auto& m = static_cast<InitialVelocityModule&>(module);
                ui::row("Mode", [&] { edit |= ui::choice("##mode", m.mode, VELOCITY_MODES); });
                if (m.mode == VelocityMode::cone) {
                    ui::row("Direction", [&] { edit |= ui::vec3("##direction", m.direction, {.speed = 0.01f}); });
                    ui::row("Angle", "Half the cone's opening.", [&] {
                        edit |= ui::drag("##angle", m.angle, {.speed = 0.5f, .min = 0.0f, .max = 90.0f, .unit = U::Degrees, .format = "%.1f"});
                    });
                    ui::subheading("Speed");
                    edit |= min_max(m.speed_min, m.speed_max, {.speed = 0.1f, .min = 0.0f, .max = 100.0f, .unit = U::MetresPerSecond});
                } else {
                    ui::row("Min", [&] { edit |= ui::vec3("##min", m.velocity_min, {.speed = 0.1f, .unit = U::MetresPerSecond}); });
                    ui::row("Max", [&] { edit |= ui::vec3("##max", m.velocity_max, {.speed = 0.1f, .unit = U::MetresPerSecond}); });
                }
                break;
            }
            case ModuleType::initial_color: {
                auto& m = static_cast<InitialColorModule&>(module);
                ui::row("Color", [&] { edit |= ui::color("##color", m.color, ui::ColorSpace::Srgb); });
                break;
            }
            case ModuleType::initial_size: {
                auto& m = static_cast<InitialSizeModule&>(module);
                ui::subheading("Size");
                edit |= min_max(m.size_min, m.size_max, {.speed = 0.001f, .min = 0.001f, .max = 10.0f, .unit = U::Metres, .format = "%.3f"});
                break;
            }
            case ModuleType::initial_stretch: {
                auto& m = static_cast<InitialStretchModule&>(module);
                ui::row("Stretch", "0 draws a square; above it the particle streaks along its velocity.", [&] {
                    edit |= ui::drag("##stretch", m.stretch, {.speed = 0.1f, .min = 0.0f, .max = 100.0f, .format = "%.1f"});
                });
                break;
            }
            case ModuleType::burst: {
                auto& m = static_cast<BurstModule&>(module);
                ui::row("Count", [&] {
                    int count = static_cast<int>(m.count);
                    const auto e = ui::drag_int("##count", count, 1.0f, 1, 100000);
                    if (e.changed) m.count = static_cast<uint32_t>(std::max(count, 1));
                    edit |= e;
                });
                break;
            }
            case ModuleType::initial_rotation: {
                auto& m = static_cast<InitialRotationModule&>(module);
                ui::subheading("Rotation");
                ui::row("Min", [&] {
                    edit |= degrees("##min", m.rotation_min, {.speed = 1.0f, .min = -360.0f, .max = 360.0f, .unit = U::Degrees, .format = "%.0f"});
                });
                ui::row("Max", [&] {
                    edit |= degrees("##max", m.rotation_max, {.speed = 1.0f, .min = -360.0f, .max = 360.0f, .unit = U::Degrees, .format = "%.0f"});
                });
                ui::row("Spin", "Degrees a second.", [&] {
                    edit |= degrees("##spin", m.speed, {.speed = 1.0f, .min = -3600.0f, .max = 3600.0f, .unit = U::Degrees, .format = "%.0f"});
                });
                ui::row("Spin variance", "How far each particle's spin differs, either way, in degrees a second.", [&] {
                    edit |= degrees("##variance", m.speed_variance, {.speed = 1.0f, .min = 0.0f, .max = 3600.0f, .unit = U::Degrees, .format = "%.0f"});
                });
                break;
            }
            case ModuleType::initial_texture:
                break;
            case ModuleType::flipbook: {
                auto& m = static_cast<FlipbookModule&>(module);
                auto whole = [&](const char* label, const char* id, uint32_t& value, int lo, int hi) {
                    ui::row(label, [&] {
                        int v = static_cast<int>(value);
                        const auto e = ui::drag_int(id, v, 0.1f, lo, hi);
                        if (e.changed) value = static_cast<uint32_t>(std::max(v, lo));
                        edit |= e;
                    });
                };
                whole("Columns", "##columns", m.columns, 1, 64);
                whole("Rows", "##rows", m.rows, 1, 64);
                whole("Frames", "##frames", m.frame_count, 0, 255);
                ui::hint("0 frames plays every cell of the grid");
                ui::row("Mode", [&] { edit |= ui::choice("##mode", m.mode, FLIPBOOK_MODES); });
                if (m.mode != FlipbookModule::Mode::over_lifetime) {
                    ui::row("Rate", "Frames a second.", [&] {
                        edit |= ui::drag("##fps", m.fps, {.speed = 0.5f, .min = 0.25f, .max = 63.75f});
                    });
                }
                char frames[64];
                const uint32_t total = m.frame_count > 0 ? m.frame_count : m.columns * m.rows;
                std::snprintf(frames, sizeof(frames), "%u frames on a %u \xc3\x97 %u grid", total, m.columns, m.rows);
                ui::row("Plays", [&] { ui::readout(frames); });
                break;
            }
            case ModuleType::velocity_inherit: {
                auto& m = static_cast<VelocityInheritanceModule&>(module);
                ui::row("Factor", "How much of the emitter's own velocity a new particle starts with.", [&] {
                    edit |= ui::slider("##factor", m.factor, 0.0f, 1.0f, U::Times);
                });
                break;
            }
            case ModuleType::emit_over_distance: {
                auto& m = static_cast<EmitOverDistanceModule&>(module);
                ui::row("Per metre", "Particles for each metre the emitter moves.", [&] {
                    edit |= ui::drag("##per_metre", m.particles_per_meter, {.speed = 0.5f, .min = 0.1f, .max = 200.0f, .format = "%.1f"});
                });
                break;
            }
            case ModuleType::gravity: {
                auto& m = static_cast<GravityModule&>(module);
                ui::row("Force", "Acceleration, in metres a second squared.", [&] {
                    edit |= ui::vec3("##force", m.force, {.speed = 0.1f});
                });
                break;
            }
            case ModuleType::color_over_life: {
                auto& m = static_cast<ColorOverLifeModule&>(module);
                ui::row("Start", [&] { edit |= ui::color("##start", m.color_start, ui::ColorSpace::Srgb); });
                ui::row("End", [&] { edit |= ui::color("##end", m.color_end, ui::ColorSpace::Srgb); });
                break;
            }
            case ModuleType::size_over_life: {
                auto& m = static_cast<SizeOverLifeModule&>(module);
                ui::row("Start", [&] {
                    edit |= ui::drag("##start", m.size_start, {.speed = 0.001f, .min = 0.001f, .max = 10.0f, .unit = U::Metres, .format = "%.3f"});
                });
                ui::row("End", [&] {
                    edit |= ui::drag("##end", m.size_end, {.speed = 0.001f, .min = 0.001f, .max = 10.0f, .unit = U::Metres, .format = "%.3f"});
                });
                break;
            }
            case ModuleType::drag: {
                auto& m = static_cast<DragModule&>(module);
                ui::row("Drag", "How fast a particle loses speed.", [&] {
                    edit |= ui::drag("##drag", m.drag, {.speed = 0.01f, .min = 0.0f, .max = 10.0f});
                });
                break;
            }
            case ModuleType::noise: {
                auto& m = static_cast<NoiseModule&>(module);
                ui::row("Strength", [&] { edit |= ui::drag("##strength", m.strength, {.speed = 0.1f, .min = 0.0f, .max = 100.0f}); });
                ui::row("Frequency", "How small the noise's swirls are: higher is finer.", [&] {
                    edit |= ui::drag("##frequency", m.frequency, {.speed = 0.1f, .min = 0.01f, .max = 50.0f});
                });
                ui::row("Scroll speed", [&] {
                    edit |= ui::drag("##scroll", m.scroll_speed, {.speed = 0.1f, .min = 0.0f, .max = 10.0f});
                });
                break;
            }
            case ModuleType::attract: {
                auto& m = static_cast<AttractModule&>(module);
                ui::row("Position", [&] { edit |= ui::vec3("##position", m.position, {.speed = 0.1f, .unit = U::Metres}); });
                ui::row("Strength", [&] { edit |= ui::drag("##strength", m.strength, {.speed = 0.1f, .min = 0.0f, .max = 100.0f}); });
                ui::row("Radius", "How far the pull reaches.", [&] {
                    edit |= ui::drag("##radius", m.radius, {.speed = 0.1f, .min = 0.01f, .max = 1000.0f, .unit = U::Metres});
                });
                break;
            }
            case ModuleType::velocity_limit: {
                auto& m = static_cast<VelocityLimitModule&>(module);
                ui::row("Max speed", [&] {
                    edit |= ui::drag("##max_speed", m.max_speed, {.speed = 0.1f, .min = 0.0f, .max = 1000.0f, .unit = U::MetresPerSecond});
                });
                break;
            }
            case ModuleType::collision: {
                auto& m = static_cast<CollisionModule&>(module);
                ui::row("Against", "Static geometry is the scene's baked distance field; the depth buffer also sees "
                                   "what moves or is displaced, but only on screen.",
                        [&] { edit |= ui::choice("##source", m.source, COLLISION_SOURCES); });
                ui::row("Response", [&] { edit |= ui::choice("##response", m.response, COLLISION_RESPONSES); });
                if (m.response != CollisionModule::Response::kill) {
                    ui::row("Restitution", "How much of its speed a particle keeps off a bounce.", [&] {
                        edit |= ui::slider("##restitution", m.restitution, 0.0f, 1.0f);
                    });
                    ui::row("Friction", [&] { edit |= ui::slider("##friction", m.friction, 0.0f, 1.0f); });
                }
                ui::row("Radius", "The particle's collision radius, as a share of its size.", [&] {
                    edit |= ui::slider("##radius", m.radius_scale, 0.0f, 2.0f, U::Times);
                });
                break;
            }
            case ModuleType::curve_color_over_life:
            case ModuleType::curve_size_over_life:
                // No rows: the curves take the panel's width, below.
                break;
            case ModuleType::wind: {
                auto& m = static_cast<WindModule&>(module);
                ui::row("Global wind", "Blow with the environment's wind, its direction, speed and gusts, times "
                                       "Strength; Direction is then not used.",
                        [&] { edit |= ui::checkbox("##global", m.use_global); });
                ui::row("Direction", [&] {
                    ImGui::BeginDisabled(m.use_global);
                    edit |= ui::vec3("##direction", m.direction, {.speed = 0.01f, .min = -1.0f, .max = 1.0f});
                    ImGui::EndDisabled();
                });
                ui::row("Strength", [&] { edit |= ui::drag("##strength", m.strength, {.speed = 0.1f, .min = 0.0f, .max = 100.0f}); });
                break;
            }
            case ModuleType::orbital: {
                auto& m = static_cast<OrbitalModule&>(module);
                ui::row("Mode", [&] { edit |= ui::choice("##mode", m.mode, ORBITAL_MODES); });
                ui::row(m.mode == OrbitalMode::orbit ? "Center" : "Axis", [&] {
                    edit |= ui::vec3("##axis", m.axis, {.speed = 0.01f, .min = -1.0f, .max = 1.0f});
                });
                ui::row("Speed", "Turn rate, in radians a second.", [&] {
                    edit |= ui::drag("##speed", m.speed, {.speed = 0.1f, .min = -20.0f, .max = 20.0f});
                });
                if (m.mode == OrbitalMode::swirl) {
                    ui::row("Pull", "How hard the swirl draws particles in.", [&] {
                        edit |= ui::drag("##pull", m.pull, {.speed = 0.1f, .min = -10.0f, .max = 10.0f});
                    });
                }
                break;
            }
            case ModuleType::emissive: {
                auto& m = static_cast<EmissiveModule&>(module);
                ui::row("Color", [&] { edit |= ui::color("##color", m.color, ui::ColorSpace::Srgb); });
                ui::row("Intensity", [&] { edit |= ui::drag("##intensity", m.intensity, {.speed = 0.1f, .min = 0.0f, .max = 50.0f}); });
                break;
            }
            case ModuleType::sub_emitter: {
                auto& m = static_cast<SubEmitterModule&>(module);
                ui::row("Trigger", [&] { edit |= ui::choice("##trigger", m.trigger, TRIGGERS); });
                ui::row("Child emitter", "The emitter, by its place in the effect, that is fired.", [&] {
                    int child = static_cast<int>(m.child_emitter_index);
                    const auto e = ui::drag_int("##child", child, 0.1f, 0, 255);
                    if (e.changed) m.child_emitter_index = static_cast<uint32_t>(std::max(child, 0));
                    edit |= e;
                });
                ui::row("Burst", "Particles fired each time.", [&] {
                    int count = static_cast<int>(m.burst_count);
                    const auto e = ui::drag_int("##burst", count, 0.2f, 1, 10000);
                    if (e.changed) m.burst_count = static_cast<uint32_t>(std::max(count, 1));
                    edit |= e;
                });
                ui::row("Velocity inherit", [&] {
                    edit |= ui::slider("##inherit", m.velocity_inherit, 0.0f, 1.0f, U::Times);
                });
                break;
            }
            case ModuleType::billboard:
                ui::row("Draws", [&] { ui::readout("A quad facing the camera"); });
                break;
            case ModuleType::trail: {
                auto& m = static_cast<TrailModule&>(module);
                ui::row("Width", [&] {
                    edit |= ui::drag("##width", m.width, {.speed = 0.001f, .min = 0.001f, .max = 10.0f, .unit = U::Metres, .format = "%.3f"});
                });
                ui::row("Width over life", "1 keeps the width to the end; 0 tapers it to nothing.", [&] {
                    edit |= ui::slider("##taper", m.width_over_life, 0.0f, 1.0f);
                });
                ui::row("Face camera", [&] { edit |= ui::checkbox("##face", m.face_camera); });
                break;
            }
            case ModuleType::lit_particle: {
                auto& m = static_cast<LitParticleModule&>(module);
                ui::row("Metallic", [&] { edit |= ui::slider("##metallic", m.metallic, 0.0f, 1.0f); });
                ui::row("Roughness", [&] { edit |= ui::slider("##roughness", m.roughness, 0.04f, 1.0f); });
                break;
            }
            case ModuleType::mesh_particle: {
                auto& m = static_cast<MeshParticleModule&>(module);
                ui::row("Shape", [&] { edit |= ui::choice("##shape", m.shape, MESH_SHAPES); });
                break;
            }
            case ModuleType::distortion: {
                auto& m = static_cast<DistortionModule&>(module);
                ui::row("Strength", [&] {
                    edit |= ui::drag("##strength", m.strength, {.speed = 0.001f, .min = 0.0f, .max = 1.0f, .format = "%.3f"});
                });
                break;
            }
        }

        // Asset references, through the engine's one asset field.
        for (auto& field : module.asset_fields()) {
            char id[64];
            std::snprintf(id, sizeof(id), "##asset%p", static_cast<const void*>(field.reference));
            ui::row(field.label, [&] {
                if (asset_slot({.label = field.label, .id = id, .extensions = field.extensions, .ctx = &ctx,
                                .shader_types = field.shader_types},
                               *field.reference)) {
                    edit = {true, true};
                }
            });
        }
    }

    // Curves take the panel's width, under the rows.
    if (module.type() == ModuleType::curve_color_over_life) {
        auto& m = static_cast<CurveColorOverLifeModule&>(module);
        const struct {
            const char* label;
            Curve* curve;
        } channels[] = {{"Red", &m.curve_r}, {"Green", &m.curve_g}, {"Blue", &m.curve_b}, {"Alpha", &m.curve_a}};
        for (const auto& channel : channels) {
            if (ui::subheading_foldable(channel.label, false)) {
                ImGui::PushID(channel.label);
                if (CurveEditor("##curve", *channel.curve, {-1, 120}, 0.0f, 1.0f)) edit = {true, true};
                ImGui::PopID();
            }
        }
        if (ui::button("Linear", ui::ButtonKind::Ghost)) {
            m.curve_r = Curve::linear(1.0f, 1.0f);
            m.curve_g = Curve::linear(0.8f, 0.1f);
            m.curve_b = Curve::linear(0.2f, 0.0f);
            m.curve_a = Curve::linear(1.0f, 0.0f);
            edit = {true, true};
        }
        ImGui::SameLine();
        if (ui::button("Fade out", ui::ButtonKind::Ghost)) {
            m.curve_r = Curve::linear(1.0f, 1.0f);
            m.curve_g = Curve::linear(1.0f, 1.0f);
            m.curve_b = Curve::linear(1.0f, 1.0f);
            m.curve_a = Curve::ease_in(1.0f, 0.0f);
            edit = {true, true};
        }
        ImGui::SameLine();
        if (ui::button("Pulse", ui::ButtonKind::Ghost)) {
            m.curve_a = Curve::bell(1.0f);
            edit = {true, true};
        }
    } else if (module.type() == ModuleType::curve_size_over_life) {
        auto& m = static_cast<CurveSizeOverLifeModule&>(module);
        edit |= curve_with_presets("##size", m.curve, std::pair{"Linear", Curve::linear(0.1f, 0.02f)},
                                   std::pair{"Shrink", Curve::ease_in(0.1f, 0.0f)},
                                   std::pair{"Grow", Curve::ease_out(0.0f, 0.1f)}, std::pair{"Pulse", Curve::bell(0.15f)});
    }
    return edit;
}

} // namespace fjell
