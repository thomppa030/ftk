#include "ui/vfx_editor_context.hpp"
#include "ui/inspectors/vfx_module_inspectors.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/component_block.hpp"
#include "ftk/ui/kit/dialog.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ui/icons.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/tabs.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "scene/component_meta.hpp"
#include "ui/asset_ref_widget.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "renderer/systems/vfx_asset.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/systems/vfx_module.hpp"
#include "renderer/systems/vfx_expression.hpp"
#include "renderer/systems/vfx_pin.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/transform.hpp"
#include "core/base_path.hpp"
#include "core/input.hpp"
#include "ftk/base/log.hpp"
#include "core/project_manager.hpp"
#include "ftk/base/string_utils.hpp"
#include "ftk/app/icon_cache.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <optional>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <imgui_internal.h>

#include <cstdio>
#include <filesystem>


namespace fjell {

namespace fs = std::filesystem;

VfxEditorContext::VfxEditorContext(VfxEditorDeps deps)
    : deps_{deps} {}

ftk::EditorContext::DockPreset VfxEditorContext::dock_preset() const {
    return {.preview = {"Preview"}, .under_preview = {"Graph", "Timeline"}, .inspector = {"Modules"}};
}

void VfxEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Preview");

    timeline_.set_title("Timeline");
    timeline_.set_category(ftk::theme::Category::Vfx);
    timeline_.set_looping(true);
    timeline_.set_duration(5.0f);
    timeline_.set_playing(true);

    timeline_connections_.push_back(timeline_.on_lane_selected.bind(
        [this](int lane_idx) {
            if (lane_idx >= 0 && lane_idx < static_cast<int>(effect_->emitters.size())) {
                selected_emitter_ = lane_idx;
            }
        }));

    timeline_connections_.push_back(timeline_.on_pause.bind([this]() {
        if (!effect_) return;
        for (auto& em : effect_->emitters) em.paused = true;
    }));

    timeline_connections_.push_back(timeline_.on_play.bind([this]() {
        if (!effect_) return;
        for (auto& em : effect_->emitters) em.paused = false;
    }));

    timeline_connections_.push_back(timeline_.on_stop.bind([this]() {
        if (!effect_) return;
        effect_->reset_playback();
    }));

    timeline_connections_.push_back(timeline_.on_lane_dragged.bind(
        [this](int lane_idx, float new_start) {
            if (!effect_ || lane_idx < 0
                || lane_idx >= static_cast<int>(effect_->emitters.size())) return;
            effect_->emitters[lane_idx].start_delay = new_start;
        }));

    timeline_connections_.push_back(timeline_.on_scrub.bind(
        [this](float time) {
            if (!effect_) return;
            // When scrubbing, set each emitter's elapsed to match the timeline position.
            // This lets the system restart emission from the correct point.
            for (auto& em : effect_->emitters) {
                em.emitter_elapsed = time;
                em.finished = false;
                em.loops_completed = 0;
                // Re-evaluate burst state: if we're past delay, burst should fire
                if (time >= em.start_delay) {
                    em.burst_fired = false;
                    em.burst_elapsed = 0.0f;
                    em.local_elapsed = 0.0f;
                }
                em.emit_accumulator = 0.0f;
            }
        }));

    graph_.set_add_module_items([this](VfxEmitter& emitter, int stack) { draw_add_module_items(emitter, stack); });
    graph_.set_module_picture([this](const VfxModule& module) -> ImTextureID {
        if (module.type() != ModuleType::initial_texture || deps_.icon_cache == nullptr) return {};
        const auto& texture = static_cast<const InitialTextureModule&>(module);
        if (texture.texture_path.empty()) return {};
        return deps_.icon_cache->thumbnail(base_path::resolve_asset(texture.texture_path, deps_.project_mgr->active()));
    });

    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

void VfxEditorContext::shutdown() {
    timeline_connections_.clear();
    deps_.vulkan_context->release_scene_particles(preview_scene_);
}

void VfxEditorContext::draw(float dt) {
#ifndef NDEBUG
    if (fjvfx_watcher_.poll() && fjvfx_changed_) {
        fjvfx_changed_ = false;
        reload_effect();
    }
#endif

    name_preview(viewport_manager_);
    timeline_.set_title(ctx_title("Timeline"));
    preview_scene_.compute_world_matrices(nullptr);
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    draw_graph();
    draw_module_stack();

    // Build timeline lanes from emitters
    if (effect_) {
        std::vector<TimelineLane> lanes;
        for (int i = 0; i < static_cast<int>(effect_->emitters.size()); ++i) {
            auto& em = effect_->emitters[i];
            TimelineLane lane;
            lane.name = em.name;
            lane.start = em.start_delay;
            lane.duration = em.effective_duration();
            lane.selected = (i == selected_emitter_);
            lanes.push_back(lane);
        }
        timeline_.set_lanes(lanes);

        // Timeline duration = total effect duration + 1s padding
        float total_dur = effect_->total_duration();
        timeline_.set_duration(std::max(total_dur + 1.0f, 2.0f));
    }

    timeline_.draw(dt * time_scale_);

    float cur_time = timeline_.time();
    if (effect_ && timeline_.playing() && cur_time < prev_timeline_time_) {
        // Timeline looped — reset timing for emitters with explicit duration,
        // so they replay their timed sequence. Don't touch emitters with no
        // duration set (they emit continuously, the particle system handles
        // burst re-fire via burst_elapsed internally).
        for (auto& em : effect_->emitters) {
            if (em.duration > 0.0f) {
                em.emitter_elapsed = em.start_delay;
                em.loops_completed = 0;
                em.burst_fired = false;
                em.burst_elapsed = 0.0f;
                em.local_elapsed = 0.0f;
                em.emit_accumulator = 0.0f;
                em.finished = false;
            }
        }
    }
    prev_timeline_time_ = cur_time;

    draw_shared_panels(dt);
}

bool VfxEditorContext::handle_shortcuts() {
    if (ImGui::IsAnyItemActive()) return false;

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        bool now_playing = !timeline_.playing();
        timeline_.set_playing(now_playing);
        if (effect_) {
            for (auto& em : effect_->emitters)
                em.paused = !now_playing;
        }
        return true;
    }

    return false;
}

std::vector<ViewportRenderRequest> VfxEditorContext::build_render_requests() {
    return preview_render_requests(viewport_manager_.build_render_requests());
}

void VfxEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

// ── Open / Save ─────────────────────────────────────────────────────────

void VfxEditorContext::open_effect(const std::string& fjvfx_path) {
    fjvfx_path_ = fjvfx_path;
    fjvfx_abs_path_ = base_path::resolve_asset(fjvfx_path, deps_.project_mgr->active());

    try {
        auto loaded = load_vfx_effect(fjvfx_abs_path_);
        resolve_vfx_textures(loaded, *deps_.resource_manager, deps_.project_mgr->active());
        effect_ = std::make_shared<VfxEffect>(std::move(loaded));
    } catch (const std::exception& e) {
        FTK_CORE_ERROR("VFX editor: failed to load {}: {}", fjvfx_path, e.what());
        return;
    }

    name_ = fs::path(fjvfx_path).stem().string();
    selected_emitter_ = effect_->emitters.empty() ? -1 : 0;
    show_emitter();

    timeline_.set_duration(std::max(effect_->max_lifetime() + 1.0f, 3.0f));
    timeline_.show(fjvfx_path);

    setup_preview();

    viewport_manager_.frame({0.0f, 1.0f, 0.0f}, 3.0f);

    document_opened();
    FTK_CORE_INFO("VFX editor opened: {}", fjvfx_path);

#ifndef NDEBUG
    fjvfx_watcher_.clear();
    fjvfx_changed_ = false;
    fjvfx_watcher_.watch(fjvfx_abs_path_, [this](const std::filesystem::path&) {
        fjvfx_changed_ = true;
    });
#endif
}

#ifndef NDEBUG
void VfxEditorContext::reload_effect() {
    if (fjvfx_abs_path_.empty()) return;
    try {
        take_effect(load_vfx_effect(fjvfx_abs_path_));
    } catch (const std::exception& e) {
        FTK_CORE_ERROR("VFX hot-reload failed for {}: {}", fjvfx_path_, e.what());
        return;
    }
    document_opened();
    FTK_CORE_INFO("VFX hot-reloaded: {}", fjvfx_path_);
}

#endif

ftk::Result<> VfxEditorContext::restore(const std::string& text) {
    try {
        take_effect(load_vfx_effect_from_json(nlohmann::json::parse(text)));
    } catch (const std::exception& e) {
        return ftk::make_error(e.what());
    }
    return {};
}

void VfxEditorContext::take_effect(VfxEffect loaded) {
    // Keep what is selected, clamped to the new effect.
    const int prev_emitter = selected_emitter_;
    resolve_vfx_textures(loaded, *deps_.resource_manager, deps_.project_mgr->active());
    effect_ = std::make_shared<VfxEffect>(std::move(loaded));

    const int emitter_count = static_cast<int>(effect_->emitters.size());
    selected_emitter_ = (prev_emitter >= 0 && prev_emitter < emitter_count)
        ? prev_emitter : (emitter_count > 0 ? 0 : -1);

    timeline_.set_duration(std::max(effect_->max_lifetime() + 1.0f, 3.0f));
    setup_preview();
}

std::string VfxEditorContext::document() const {
    return effect_ ? vfx_effect_text(*effect_) : std::string{};
}

ftk::Result<> VfxEditorContext::write() {
    if (fjvfx_abs_path_.empty() || !effect_) return ftk::make_error("no effect is open");

    try {
        save_vfx_effect(fjvfx_abs_path_, *effect_);
    } catch (const std::exception& e) {
        return ftk::make_error(e.what());
    }
    FTK_CORE_INFO("VFX saved: {}", fjvfx_abs_path_);
    return {};
}

void VfxEditorContext::setup_preview() {
    preview_scene_.clear();

    if (!effect_) return;

    auto node = std::make_unique<SceneNode>();
    node->name = "vfx_preview";
    auto& ec = node->components.add<EmitterComponent>();
    ec.effect = effect_;
    ec.vfx_path = fjvfx_path_;

    preview_node_id_ = preview_scene_.add_node(std::move(node));
    preview_scene_.compute_world_matrices(nullptr);
}

// ── Graph ────────────────────────────────────────────────────────────────

void VfxEditorContext::show_emitter() {
    graph_.show(fjvfx_path_ + "|" + std::to_string(selected_emitter_));
}

void VfxEditorContext::draw_graph() {
    auto panel = ftk::ui::Panel(icon::vfx, ctx_title("Graph").c_str());
    if (!panel) return;
    if (!effect_) {
        ftk::ui::empty_state(icon::vfx, "No effect", "The effect couldn't be read; the console says why");
        return;
    }

    // The emitters as tabs, the one the graph shows in front (sheet 25).
    std::vector<std::string> names;
    for (const auto& em : effect_->emitters) names.push_back(em.name);
    const auto tabs = ftk::ui::tab_strip("##emitters", {.names = names,
                                                   .selected = std::max(selected_emitter_, 0),
                                                   .category = ftk::theme::Category::Vfx,
                                                   .add_label = "Add emitter",
                                                   .remove_label = "Remove emitter"});
    if (tabs.selected) {
        selected_emitter_ = *tabs.selected;
        show_emitter();
    }
    if (tabs.add) {
        effect_->emitters.push_back(VfxEmitter::make_default());
        auto& em = effect_->emitters.back();
        em.name = "emitter_" + std::to_string(effect_->emitters.size());
        selected_emitter_ = static_cast<int>(effect_->emitters.size()) - 1;
        show_emitter();
        setup_preview();
    }
    if (tabs.removed) {
        effect_->emitters.erase(effect_->emitters.begin() + *tabs.removed);
        selected_emitter_ = std::min(selected_emitter_, static_cast<int>(effect_->emitters.size()) - 1);
        show_emitter();
        setup_preview();
    }
    if (tabs.rename) {
        renaming_emitter_ = *tabs.rename;
        rename_text_ = effect_->emitters[static_cast<size_t>(*tabs.rename)].name;
        ftk::ui::open_dialog("##rename_emitter");
    }
    if (ftk::ui::prompt_dialog("##rename_emitter", {.title = "Rename emitter", .hint = "Its name", .confirm = "Rename"},
                          rename_text_) == ftk::ui::DialogAnswer::Confirm &&
        renaming_emitter_ >= 0 && renaming_emitter_ < static_cast<int>(effect_->emitters.size())) {
        effect_->emitters[static_cast<size_t>(renaming_emitter_)].name = rename_text_;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preview speed");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    (void)ftk::ui::slider("##speed", time_scale_, 0.0f, 3.0f, ftk::ui::Unit::Times, "%.1f");

    if (selected_emitter_ < 0 || selected_emitter_ >= static_cast<int>(effect_->emitters.size())) {
        ftk::ui::empty_state(icon::vfx, "No emitter selected", "Pick one above or add one");
        return;
    }
    (void)graph_.draw(effect_->emitters[static_cast<size_t>(selected_emitter_)]);
}

// ── Modules ──────────────────────────────────────────────────────────────

namespace {

// Every module an emitter can hold: the stack it goes in and its name in
// the Add menus.
struct ModuleKind {
    int stack;  // 0 spawn, 1 update, 2 render
    const char* label;
    std::unique_ptr<VfxModule> (*make)();
};

template <typename T>
std::unique_ptr<VfxModule> make_module() {
    return std::make_unique<T>();
}

constexpr ModuleKind MODULE_KINDS[] = {
    {0, "Spawn rate", make_module<SpawnRateModule>},
    {0, "Initial lifetime", make_module<InitialLifetimeModule>},
    {0, "Initial position", make_module<InitialPositionModule>},
    {0, "Initial velocity", make_module<InitialVelocityModule>},
    {0, "Initial color", make_module<InitialColorModule>},
    {0, "Initial size", make_module<InitialSizeModule>},
    {0, "Burst", make_module<BurstModule>},
    {0, "Emit over distance", make_module<EmitOverDistanceModule>},
    {0, "Initial rotation", make_module<InitialRotationModule>},
    {0, "Initial texture", make_module<InitialTextureModule>},
    {0, "Flipbook", make_module<FlipbookModule>},
    {0, "Velocity inheritance", make_module<VelocityInheritanceModule>},
    {1, "Gravity", make_module<GravityModule>},
    {1, "Color over life", make_module<ColorOverLifeModule>},
    {1, "Size over life", make_module<SizeOverLifeModule>},
    {1, "Drag", make_module<DragModule>},
    {1, "Noise", make_module<NoiseModule>},
    {1, "Attract", make_module<AttractModule>},
    {1, "Velocity limit", make_module<VelocityLimitModule>},
    {1, "Collision", make_module<CollisionModule>},
    {1, "Wind", make_module<WindModule>},
    {1, "Orbital", make_module<OrbitalModule>},
    {1, "Emissive", make_module<EmissiveModule>},
    {1, "Sub-emitter", make_module<SubEmitterModule>},
    {1, "Color over life, curves", make_module<CurveColorOverLifeModule>},
    {1, "Size over life, curve", make_module<CurveSizeOverLifeModule>},
    {2, "Billboard", make_module<BillboardModule>},
    {2, "Trail", make_module<TrailModule>},
    {2, "Lit particle", make_module<LitParticleModule>},
    {2, "Mesh particle", make_module<MeshParticleModule>},
    {2, "Distortion", make_module<DistortionModule>},
};

constexpr const char* STACK_NAMES[] = {"Spawn", "Update", "Render"};

} // namespace

std::vector<std::unique_ptr<VfxModule>>& VfxEditorContext::stack_of(VfxEmitter& emitter, int stack) {
    return stack == 0 ? emitter.spawn_modules : stack == 1 ? emitter.update_modules : emitter.render_modules;
}

void VfxEditorContext::draw_add_module_items(VfxEmitter& emitter, int stack) {
    for (const auto& kind : MODULE_KINDS) {
        if (kind.stack != stack || !ftk::ui::menu_item({.label = kind.label})) continue;
        auto& modules = stack_of(emitter, stack);
        modules.push_back(kind.make());
        graph_.select_module(stack, static_cast<int>(modules.size()) - 1);
    }
}

// ── Module Stack ────────────────────────────────────────────────────────

void VfxEditorContext::resolve_module_assets(VfxModule& mod) {
    const auto* project = deps_.project_mgr->active();

    if (mod.type() == ModuleType::initial_texture) {
        auto& tex = static_cast<InitialTextureModule&>(mod);
        tex.bindless_index = 0;
        if (tex.texture_path.empty()) return;
        auto resolved = base_path::resolve_asset(tex.texture_path, project);
        if (resolved.empty()) return;
        if (auto result = deps_.resource_manager->load_texture(resolved)) {
            tex.bindless_index = deps_.resource_manager->bindless_index(*result);
        }
        return;
    }

    if (mod.type() == ModuleType::mesh_particle) {
        // Cleared so the particle system re-resolves it against the new path.
        static_cast<MeshParticleModule&>(mod).resolved_material_id = 0;
    }
}

void VfxEditorContext::draw_module_stack() {
    auto panel = ftk::ui::Panel(icon::vfx, ctx_title("Modules").c_str());
    if (!panel) return;
    if (!effect_ || selected_emitter_ < 0 || selected_emitter_ >= static_cast<int>(effect_->emitters.size())) {
        ftk::ui::empty_state(icon::vfx, "No emitter selected", "Select an emitter in the graph");
        return;
    }
    auto& emitter = effect_->emitters[static_cast<size_t>(selected_emitter_)];

    ftk::ui::section("Emitter", icon::vfx);
    if (auto t = ftk::ui::PropertyTable("##emitter")) {
        ftk::ui::row("Name", [&] { (void)ftk::ui::text_field("##name", emitter.name); });
        ftk::ui::row("Max particles", [&] {
            int max_particles = static_cast<int>(emitter.max_particles);
            if (ftk::ui::drag_int("##max", max_particles, 10.0f, 100, 1000000).changed) {
                emitter.max_particles = static_cast<uint32_t>(std::max(100, max_particles));
            }
        });
        static constexpr ftk::ui::Choice<ParticleBlendMode> BLENDS[] = {
            {ParticleBlendMode::additive, "Additive"}, {ParticleBlendMode::alpha, "Alpha"},
            {ParticleBlendMode::opaque, "Opaque"}};
        ftk::ui::row("Blend", [&] { (void)ftk::ui::choice("##blend", emitter.blend_mode, BLENDS); });
        ftk::ui::row("Start delay", [&] {
            (void)ftk::ui::drag("##delay", emitter.start_delay,
                           {.speed = 0.05f, .min = 0.0f, .max = 60.0f, .unit = ftk::ui::Unit::Seconds});
        });
        char lifetime[80];
        std::snprintf(lifetime, sizeof(lifetime), "How long it emits. 0 runs for a particle's lifetime, %.1f s.",
                      emitter.lifetime_max());
        ftk::ui::row("Duration", lifetime, [&] {
            (void)ftk::ui::drag("##duration", emitter.duration,
                           {.speed = 0.05f, .min = 0.0f, .max = 60.0f, .unit = ftk::ui::Unit::Seconds});
        });
        ftk::ui::row("Loops", "How many times it runs. 0 loops for ever.", [&] {
            int loops = static_cast<int>(emitter.loop_count);
            if (ftk::ui::drag_int("##loops", loops, 0.2f, 0, 1000).changed) {
                emitter.loop_count = static_cast<uint32_t>(std::max(0, loops));
            }
        });
        ftk::ui::row("Own time", "Off, the graph's Time node reads the engine's clock, so a sin- or cos-driven value "
                            "lands on another phase each time the emitter fires. On, each firing starts its own "
                            "Time at 0, so the graph replays the same way every time.",
                [&] { (void)ftk::ui::checkbox("##instanced_time", emitter.instanced_time); });
        char now[96];
        std::snprintf(now, sizeof(now), "%.2f s, loop %u, %s", emitter.emitter_elapsed, emitter.loops_completed,
                      emitter.finished ? "finished" : emitter.paused ? "paused" : "playing");
        ftk::ui::row("Now", [&] { ftk::ui::readout(now); });
    }

    InspectorContext ictx{};
    ictx.resource_manager = deps_.resource_manager;
    ictx.project_mgr = deps_.project_mgr;
    ictx.content_browser = deps_.content_browser;
    ictx.icons = deps_.icon_cache;

    for (int stack = 0; stack < 3; ++stack) {
        auto& modules = stack_of(emitter, stack);
        ftk::ui::section(STACK_NAMES[stack], icon::vfx);
        std::optional<std::size_t> remove;
        for (std::size_t i = 0; i < modules.size(); ++i) {
            VfxModule& module = *modules[i];
            ImGui::PushID(stack);
            ImGui::PushID(static_cast<int>(i));
            bool removed = false;
            if (ftk::ui::component_block(module.name(), ftk::theme::Category::Vfx, removed, module.enabled)) {
                ftk::ui::FadedBody faded(!module.enabled);
                if (draw_vfx_module_fields(module, ictx).committed) resolve_module_assets(module);
            }
            if (removed) remove = i;
            ImGui::PopID();
            ImGui::PopID();
        }
        if (remove) {
            modules.erase(modules.begin() + static_cast<std::ptrdiff_t>(*remove));
            emitter.expression_graph.module_removed(stack, static_cast<int>(*remove));
            graph_.clear_selection();
        }
        // Each stack adds its own kind of module.
        ImGui::PushID(stack);
        const std::string add = std::string("Add ") + (stack == 0 ? "spawn" : stack == 1 ? "update" : "render") +
                                " module";
        if (ftk::ui::action(icon::add, add.c_str(), ftk::ui::ButtonKind::Ghost)) ImGui::OpenPopup("##add_to_stack");
        if (ImGui::BeginPopup("##add_to_stack")) {
            draw_add_module_items(emitter, stack);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

} // namespace fjell
