#include "ui/vfx_editor_context.hpp"
#include "renderer/particle_emitter.hpp"
#include "renderer/vfx_asset.hpp"
#include "renderer/vfx_emitter.hpp"
#include "renderer/vfx_module.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/resource_manager.hpp"
#include "core/base_path.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "ui/icon_cache.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

VfxEditorContext::VfxEditorContext(VfxEditorDeps deps)
    : deps_{deps} {}

void VfxEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "VFX Preview");
}

void VfxEditorContext::shutdown() {
    viewport_manager_.release_textures();
}

void VfxEditorContext::draw(float dt) {
    if (playing_) {
        preview_scene_.compute_world_matrices(nullptr);
    }
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    draw_emitter_list();
    draw_module_stack();
    draw_module_palette();
}

void VfxEditorContext::setup_dockspace(ImGuiID main_area) {
    ImGuiID center = main_area;

    // Left: Emitter list — 14%
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.14f, nullptr, &center);

    // Right: Module stack + palette — 28%
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
    ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.35f, nullptr, &right);

    // Bottom: stats/console — 25%
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);

    int ci = context_index();
    char stats[32], console[32], history[32];
    std::snprintf(stats, sizeof(stats), "Stats##ctx%d", ci);
    std::snprintf(console, sizeof(console), "Console##ctx%d", ci);
    std::snprintf(history, sizeof(history), "History##ctx%d", ci);

    ImGui::DockBuilderDockWindow("Emitters", left);
    ImGui::DockBuilderDockWindow("VFX Preview", center);
    ImGui::DockBuilderDockWindow("Module Stack", right);
    ImGui::DockBuilderDockWindow("Add Module", right_bottom);
    ImGui::DockBuilderDockWindow(history, right_bottom);
    ImGui::DockBuilderDockWindow(stats, bottom);
    ImGui::DockBuilderDockWindow(console, bottom);
}

bool VfxEditorContext::handle_shortcuts() {
    if (ImGui::GetIO().WantTextInput) return false;

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        playing_ = !playing_;
        return true;
    }

    // Ctrl+S: save
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        save_effect();
        return true;
    }

    return false;
}

std::vector<ViewportRenderRequest> VfxEditorContext::build_render_requests() {
    return viewport_manager_.build_render_requests();
}

void VfxEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

bool VfxEditorContext::any_viewport_hovered() const {
    return viewport_manager_.any_hovered();
}

// ── Open / Save ──────────────────────────────────────────────────────────

void VfxEditorContext::open_effect(const std::string& fjvfx_path) {
    fjvfx_path_ = fjvfx_path;
    fjvfx_abs_path_ = base_path::resolve_asset(fjvfx_path, deps_.project_mgr->active());

    try {
        auto loaded = load_vfx_effect(fjvfx_abs_path_);
        effect_ = std::make_shared<VfxEffect>(std::move(loaded));
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("VFX editor: failed to load {}: {}", fjvfx_path, e.what());
        return;
    }

    name_ = fs::path(fjvfx_path).stem().string();
    selected_emitter_ = 0;
    setup_preview();

    if (auto* cam = viewport_manager_.active_camera()) {
        cam->focus({0.0f, 1.0f, 0.0f}, 3.0f);
    }

    FJELL_CORE_INFO("VFX editor opened: {}", fjvfx_path);
}

void VfxEditorContext::save_effect() {
    if (fjvfx_abs_path_.empty() || !effect_) return;
    save_vfx_effect(fjvfx_abs_path_, *effect_);
    FJELL_CORE_INFO("VFX saved: {}", fjvfx_abs_path_);
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

// ── Emitter List ─────────────────────────────────────────────────────────

void VfxEditorContext::draw_emitter_list() {
    if (ImGui::Begin("Emitters")) {
        if (!effect_) {
            ImGui::TextDisabled("No effect loaded");
            ImGui::End();
            return;
        }

        for (int i = 0; i < static_cast<int>(effect_->emitters.size()); ++i) {
            auto& emitter = effect_->emitters[i];
            ImGui::PushID(i);

            bool selected = (i == selected_emitter_);
            if (ImGui::Selectable(emitter.name.c_str(), selected)) {
                selected_emitter_ = i;
            }
            ImGui::PopID();
        }

        ImGui::Separator();

        if (ImGui::Button("+ Add Emitter")) {
            effect_->emitters.push_back(VfxEmitter::make_default());
            effect_->emitters.back().name = "emitter_" + std::to_string(effect_->emitters.size());
            selected_emitter_ = static_cast<int>(effect_->emitters.size()) - 1;
            setup_preview();
        }

        if (effect_->emitters.size() > 1) {
            ImGui::SameLine();
            if (ImGui::Button("- Remove") && selected_emitter_ < static_cast<int>(effect_->emitters.size())) {
                effect_->emitters.erase(effect_->emitters.begin() + selected_emitter_);
                if (selected_emitter_ >= static_cast<int>(effect_->emitters.size())) {
                    selected_emitter_ = static_cast<int>(effect_->emitters.size()) - 1;
                }
                setup_preview();
            }
        }

        // Playback controls
        ImGui::Separator();
        if (ImGui::Button(playing_ ? "Pause" : "Play")) {
            playing_ = !playing_;
        }
        ImGui::SameLine();
        if (ImGui::Button("Restart")) {
            setup_preview();
        }
        ImGui::SliderFloat("Speed", &time_scale_, 0.0f, 3.0f);

        // Save button
        ImGui::Separator();
        if (ImGui::Button("Save (Ctrl+S)")) {
            save_effect();
        }
    }
    ImGui::End();
}

// ── Module Stack ─────────────────────────────────────────────────────────

void VfxEditorContext::draw_module_entry(VfxModule* mod, int index,
                                          std::vector<std::unique_ptr<VfxModule>>& stack,
                                          const char* /*stack_label*/) {
    ImGui::PushID(index);

    // Header: checkbox + name + remove button
    ImGui::Checkbox("##en", &mod->enabled);
    ImGui::SameLine();

    bool open = ImGui::TreeNodeEx(mod->name(),
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);

    // Remove button (right-aligned)
    float btn_w = ImGui::CalcTextSize("X").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetWindowWidth() - btn_w - 10);
    if (ImGui::SmallButton("X")) {
        stack.erase(stack.begin() + index);
        if (open) ImGui::TreePop();
        ImGui::PopID();
        return;
    }

    if (open) {
        if (mod->enabled) {
            mod->draw_inspector();
        } else {
            ImGui::TextDisabled("(disabled)");
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void VfxEditorContext::draw_module_stack() {
    if (ImGui::Begin("Module Stack")) {
        if (!effect_ || effect_->emitters.empty()) {
            ImGui::TextDisabled("No emitter selected");
            ImGui::End();
            return;
        }

        if (selected_emitter_ >= static_cast<int>(effect_->emitters.size())) {
            selected_emitter_ = 0;
        }
        auto& emitter = effect_->emitters[selected_emitter_];

        // Emitter name + settings
        char name_buf[128];
        std::snprintf(name_buf, sizeof(name_buf), "%s", emitter.name.c_str());
        if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
            emitter.name = name_buf;
        }

        int max_p = static_cast<int>(emitter.max_particles);
        if (ImGui::InputInt("Max Particles", &max_p, 100, 1000)) {
            emitter.max_particles = static_cast<uint32_t>(std::max(100, max_p));
        }

        const char* blend_modes[] = {"Additive", "Alpha"};
        int bm = static_cast<int>(emitter.blend_mode);
        if (ImGui::Combo("Blend Mode", &bm, blend_modes, 2)) {
            emitter.blend_mode = static_cast<ParticleBlendMode>(bm);
        }

        ImGui::Separator();

        // ── Spawn stack ──
        ImGui::SeparatorText("Spawn");
        for (int i = 0; i < static_cast<int>(emitter.spawn_modules.size()); ++i) {
            draw_module_entry(emitter.spawn_modules[i].get(), i,
                              emitter.spawn_modules, "spawn");
        }

        // ── Update stack ──
        ImGui::SeparatorText("Update");
        for (int i = 0; i < static_cast<int>(emitter.update_modules.size()); ++i) {
            draw_module_entry(emitter.update_modules[i].get(), i,
                              emitter.update_modules, "update");
        }

        // ── Render stack ──
        ImGui::SeparatorText("Render");
        for (int i = 0; i < static_cast<int>(emitter.render_modules.size()); ++i) {
            draw_module_entry(emitter.render_modules[i].get(), i,
                              emitter.render_modules, "render");
        }
    }
    ImGui::End();
}

// ── Module Palette ───────────────────────────────────────────────────────

void VfxEditorContext::draw_module_palette() {
    if (ImGui::Begin("Add Module")) {
        if (!effect_ || effect_->emitters.empty()) {
            ImGui::TextDisabled("No emitter selected");
            ImGui::End();
            return;
        }

        if (selected_emitter_ >= static_cast<int>(effect_->emitters.size())) {
            selected_emitter_ = 0;
        }
        auto& emitter = effect_->emitters[selected_emitter_];

        auto add_btn = [](const char* label, auto make_fn, auto& stack) {
            if (ImGui::Button(label, {-1, 0})) {
                stack.push_back(make_fn());
            }
        };

        ImGui::SeparatorText("Spawn");
        add_btn("Spawn Rate",       []{ return std::make_unique<SpawnRateModule>(); },        emitter.spawn_modules);
        add_btn("Initial Lifetime", []{ return std::make_unique<InitialLifetimeModule>(); },  emitter.spawn_modules);
        add_btn("Initial Position", []{ return std::make_unique<InitialPositionModule>(); },  emitter.spawn_modules);
        add_btn("Initial Velocity", []{ return std::make_unique<InitialVelocityModule>(); },  emitter.spawn_modules);
        add_btn("Initial Color",    []{ return std::make_unique<InitialColorModule>(); },     emitter.spawn_modules);
        add_btn("Initial Size",     []{ return std::make_unique<InitialSizeModule>(); },      emitter.spawn_modules);

        ImGui::SeparatorText("Update");
        add_btn("Gravity",          []{ return std::make_unique<GravityModule>(); },          emitter.update_modules);
        add_btn("Color Over Life",  []{ return std::make_unique<ColorOverLifeModule>(); },    emitter.update_modules);
        add_btn("Size Over Life",   []{ return std::make_unique<SizeOverLifeModule>(); },     emitter.update_modules);
        add_btn("Drag",             []{ return std::make_unique<DragModule>(); },             emitter.update_modules);
        add_btn("Noise",            []{ return std::make_unique<NoiseModule>(); },            emitter.update_modules);
        add_btn("Attract",          []{ return std::make_unique<AttractModule>(); },          emitter.update_modules);
        add_btn("Velocity Limit",   []{ return std::make_unique<VelocityLimitModule>(); },   emitter.update_modules);

        ImGui::SeparatorText("Render");
        add_btn("Billboard",        []{ return std::make_unique<BillboardModule>(); },        emitter.render_modules);
    }
    ImGui::End();
}

} // namespace fjell
