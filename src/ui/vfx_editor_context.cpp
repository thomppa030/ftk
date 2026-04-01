#include "ui/vfx_editor_context.hpp"
#include "ui/dockspace_layout.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "renderer/systems/vfx_asset.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/systems/vfx_module.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/transform.hpp"
#include "core/base_path.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "ui/icon_cache.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imnodes.h>

#include <cstdio>
#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

// ── VfxTimeline ──────────────────────────────────────────────────────────

void VfxTimeline::draw_lanes(float bar_x, float bar_y, float bar_width,
                              float lane_height) {
    // Draw base lanes first
    TimelineWidget::draw_lanes(bar_x, bar_y, bar_width, lane_height);

    // Click detection on lanes
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mouse = ImGui::GetMousePos();
        for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {
            float y = bar_y + static_cast<float>(i) * lane_height;
            if (mouse.x >= bar_x && mouse.x <= bar_x + bar_width &&
                mouse.y >= y && mouse.y <= y + lane_height) {
                selected_lane_ = i;
                on_lane_selected.broadcast(i);
                break;
            }
        }
    }
}

VfxEditorContext::VfxEditorContext(VfxEditorDeps deps)
    : deps_{deps} {}

void VfxEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "VFX Preview");

    timeline_.set_title("VFX Timeline");
    timeline_.set_looping(true);
    timeline_.set_duration(5.0f);
    timeline_.set_playing(true);

    timeline_connections_.push_back(timeline_.on_lane_selected.bind(
        [this](int lane_idx) {
            if (lane_idx >= 0 && lane_idx < static_cast<int>(effect_->emitters.size())) {
                selected_emitter_ = lane_idx;
                // Also select the corresponding node in imnodes
                for (const auto& en : emitter_nodes_) {
                    if (en.emitter_index == lane_idx) {
                        ImNodes::ClearNodeSelection();
                        ImNodes::SelectNode(en.node_id);
                        break;
                    }
                }
            }
        }));

    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

void VfxEditorContext::shutdown() {
    timeline_connections_.clear();
    if (imnodes_ctx_) {
        ImNodes::DestroyContext(static_cast<ImNodesContext*>(imnodes_ctx_));
        imnodes_ctx_ = nullptr;
    }
    nodes_initialized_ = false;
    viewport_manager_.release_textures();
}

void VfxEditorContext::draw(float dt) {
    // Update window titles with context suffix
    if (auto* vp = viewport_manager_.active_panel()) {
        auto t = ctx_title("VFX Preview");
        if (vp->title != t) vp->title = std::move(t);
    }
    timeline_.set_title(ctx_title("VFX Timeline"));
    preview_scene_.compute_world_matrices(nullptr);
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    draw_node_editor();
    draw_module_stack();
    draw_module_palette();

    // Build timeline lanes from emitters
    if (effect_) {
        // Emitter colors: cycle through distinct hues
        static const uint32_t lane_colors[] = {
            IM_COL32(220, 120, 50, 180),   // orange (flames)
            IM_COL32(100, 180, 220, 180),  // blue (embers)
            IM_COL32(140, 140, 140, 180),  // gray (smoke)
            IM_COL32(180, 100, 220, 180),  // purple
            IM_COL32(100, 220, 120, 180),  // green
            IM_COL32(220, 200, 80, 180),   // yellow
        };
        std::vector<TimelineLane> lanes;
        for (int i = 0; i < static_cast<int>(effect_->emitters.size()); ++i) {
            auto& em = effect_->emitters[i];
            TimelineLane lane;
            lane.name = em.name;
            lane.start = 0.0f; // TODO: per-emitter start delay
            lane.duration = em.lifetime_max();
            lane.color = lane_colors[i % 6];
            lane.selected = (i == selected_emitter_);
            lanes.push_back(lane);
        }
        timeline_.set_lanes(lanes);
    }

    timeline_.draw(dt * time_scale_);

    draw_shared_panels(dt);
}

std::vector<std::string> VfxEditorContext::docked_window_names() const {
    return {ctx_title("VFX Preview"), ctx_title("Module Stack"), ctx_title("Add Module"),
            ctx_title("VFX Graph"), ctx_title("VFX Timeline"),
            ctx_title("Stats"), ctx_title("Console"), ctx_title("History")};
}

void VfxEditorContext::setup_dockspace(ImGuiID main_area) {
    enum class EPanel { Preview, ModuleStack, AddModule, Graph, Timeline, Stats, Console, History };
    using N = DockNode;
    static const auto layout = N::horizontal_split(0.76f,
        N::vertical_split(0.60f,
            N::leaf({EPanel::Preview}),
            N::vertical_split(0.70f,
                N::leaf({EPanel::Graph}),
                N::leaf({EPanel::Timeline, EPanel::Stats, EPanel::Console, EPanel::History}))),
        N::vertical_split(0.65f,
            N::leaf({EPanel::ModuleStack}),
            N::leaf({EPanel::AddModule})));
    build_dockspace(main_area, layout, docked_window_names());
}

bool VfxEditorContext::handle_shortcuts() {
    if (ImGui::GetIO().WantTextInput) return false;

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        timeline_.set_playing(!timeline_.playing());
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
    selected_emitter_ = effect_->emitters.empty() ? -1 : 0;

    // Create own imnodes context (separate from scene editor's NodeEditorPanel)
    if (!nodes_initialized_) {
        auto* prev_ctx = ImNodes::GetCurrentContext();
        imnodes_ctx_ = static_cast<void*>(ImNodes::CreateContext());
        // Restore scene editor's context as the global default
        ImNodes::SetCurrentContext(prev_ctx);

        ImNodesStyle& style = ImNodes::GetStyle();
        style.Flags |= ImNodesStyleFlags_GridSnapping;
        style.Colors[ImNodesCol_NodeBackground]         = IM_COL32(43, 45, 49, 255);
        style.Colors[ImNodesCol_NodeBackgroundHovered]   = IM_COL32(49, 51, 56, 255);
        style.Colors[ImNodesCol_NodeBackgroundSelected]  = IM_COL32(56, 58, 64, 255);
        style.Colors[ImNodesCol_TitleBar]                = IM_COL32(30, 31, 34, 255);
        style.Colors[ImNodesCol_TitleBarHovered]         = IM_COL32(49, 51, 56, 255);
        style.Colors[ImNodesCol_TitleBarSelected]        = IM_COL32(212, 160, 84, 255);
        style.Colors[ImNodesCol_Link]                    = IM_COL32(212, 160, 84, 200);
        style.Colors[ImNodesCol_LinkHovered]             = IM_COL32(212, 160, 84, 255);
        style.Colors[ImNodesCol_LinkSelected]            = IM_COL32(184, 137, 62, 255);
        style.Colors[ImNodesCol_Pin]                     = IM_COL32(212, 160, 84, 200);
        style.Colors[ImNodesCol_PinHovered]              = IM_COL32(212, 160, 84, 255);
        style.Colors[ImNodesCol_GridBackground]          = IM_COL32(17, 18, 20, 255);
        style.Colors[ImNodesCol_GridLine]                = IM_COL32(43, 45, 49, 100);
        style.Colors[ImNodesCol_GridLinePrimary]         = IM_COL32(43, 45, 49, 180);
        style.NodeCornerRounding = 6.0f;
        style.NodePadding        = ImVec2(10.0f, 6.0f);
        style.LinkThickness      = 2.5f;
        style.PinCircleRadius    = 4.5f;

        nodes_initialized_ = true;
    }

    // Build emitter nodes
    emitter_nodes_.clear();
    node_id_counter_ = 0;
    for (int i = 0; i < static_cast<int>(effect_->emitters.size()); ++i) {
        EmitterNode en;
        en.node_id = ++node_id_counter_;
        en.emitter_index = i;
        en.output_pin_id = ++node_id_counter_;
        emitter_nodes_.push_back(en);
    }

    // Set timeline duration from max lifetime
    timeline_.set_duration(std::max(effect_->max_lifetime() + 1.0f, 3.0f));

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

bool VfxEditorContext::save() {
    if (fjvfx_path_.empty()) return false;
    save_effect();
    return true;
}

void VfxEditorContext::setup_preview() {
    preview_scene_.clear();

    if (!effect_) return;

    // Dummy ground plane (particle render needs at least one gathered mesh)
    auto plane = deps_.resource_manager->create_primitive("plane");
    if (plane) {
        auto tex = deps_.resource_manager->load_texture(
            base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
        auto mat = deps_.resource_manager->create_material("vfx_ground", tex);
        Transform ground_tf;
        ground_tf.position = {0.0f, -0.01f, 0.0f};
        ground_tf.scale = {3.0f, 1.0f, 3.0f};
        preview_scene_.add(*plane, mat, *deps_.resource_manager,
                           ground_tf, "ground", "primitive:plane");
    }

    auto node = std::make_unique<SceneNode>();
    node->name = "vfx_preview";
    auto& ec = node->components.add<EmitterComponent>();
    ec.effect = effect_;
    ec.vfx_path = fjvfx_path_;

    preview_node_id_ = preview_scene_.add_node(std::move(node));
    preview_scene_.compute_world_matrices(nullptr);
}

// ── Node Editor ──────────────────────────────────────────────────────────

void VfxEditorContext::draw_node_editor() {
    if (ImGui::Begin(ctx_title("VFX Graph").c_str())) {
        if (!effect_ || !nodes_initialized_ || !imnodes_ctx_) {
            ImGui::TextDisabled("No effect loaded");
            ImGui::End();
            return;
        }

        // Toolbar
        if (ImGui::SmallButton("+ Emitter")) {
            effect_->emitters.push_back(VfxEmitter::make_default());
            auto& em = effect_->emitters.back();
            em.name = "emitter_" + std::to_string(effect_->emitters.size());

            EmitterNode en;
            en.node_id = ++node_id_counter_;
            en.emitter_index = static_cast<int>(effect_->emitters.size()) - 1;
            en.output_pin_id = ++node_id_counter_;
            emitter_nodes_.push_back(en);

            selected_emitter_ = en.emitter_index;
            setup_preview();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Save (Ctrl+S)")) {
            save_effect();
        }
        ImGui::SameLine();
        ImGui::SliderFloat("Speed", &time_scale_, 0.0f, 3.0f, "%.1f");

        auto* prev_imnodes = ImNodes::GetCurrentContext();
        ImNodes::SetCurrentContext(static_cast<ImNodesContext*>(imnodes_ctx_));
        ImNodes::BeginNodeEditor();

        // Draw emitter nodes
        for (auto& en : emitter_nodes_) {
            if (en.emitter_index >= static_cast<int>(effect_->emitters.size())) continue;
            auto& emitter = effect_->emitters[en.emitter_index];

            bool is_selected = (en.emitter_index == selected_emitter_);

            // Color selected node's title bar
            if (is_selected) {
                ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(212, 160, 84, 255));
                ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, IM_COL32(232, 180, 104, 255));
            }

            ImNodes::BeginNode(en.node_id);

            ImNodes::BeginNodeTitleBar();
            ImGui::TextUnformatted(emitter.name.c_str());
            ImNodes::EndNodeTitleBar();

            // Node body: summary info
            const char* blend_str = emitter.blend_mode == ParticleBlendMode::additive
                ? "Additive" : "Alpha";
            ImGui::TextDisabled("%u particles | %s", emitter.max_particles, blend_str);
            ImGui::TextDisabled("%.0f/s | %.1f-%.1fs",
                                emitter.emission_rate(),
                                emitter.lifetime_min(),
                                emitter.lifetime_max());

            // Module count summary
            ImGui::TextDisabled("S:%d U:%d R:%d",
                                static_cast<int>(emitter.spawn_modules.size()),
                                static_cast<int>(emitter.update_modules.size()),
                                static_cast<int>(emitter.render_modules.size()));

            // Output pin (for future linking)
            ImNodes::BeginOutputAttribute(en.output_pin_id);
            ImGui::Text("out");
            ImNodes::EndOutputAttribute();

            ImNodes::EndNode();

            if (is_selected) {
                ImNodes::PopColorStyle();
                ImNodes::PopColorStyle();
            }
        }

        ImNodes::EndNodeEditor();

        // Handle node selection
        int selected_node = -1;
        if (ImNodes::NumSelectedNodes() > 0) {
            int sel_id = 0;
            ImNodes::GetSelectedNodes(&sel_id);
            for (const auto& en : emitter_nodes_) {
                if (en.node_id == sel_id) {
                    selected_node = en.emitter_index;
                    break;
                }
            }
        }
        if (selected_node >= 0) {
            selected_emitter_ = selected_node;
        }

        // Handle delete key
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && selected_emitter_ >= 0 &&
            effect_->emitters.size() > 1) {
            effect_->emitters.erase(effect_->emitters.begin() + selected_emitter_);
            emitter_nodes_.erase(
                std::remove_if(emitter_nodes_.begin(), emitter_nodes_.end(),
                    [this](const EmitterNode& en) { return en.emitter_index == selected_emitter_; }),
                emitter_nodes_.end());
            // Reindex
            for (auto& en : emitter_nodes_) {
                if (en.emitter_index > selected_emitter_) {
                    en.emitter_index--;
                }
            }
            selected_emitter_ = std::min(selected_emitter_,
                static_cast<int>(effect_->emitters.size()) - 1);
            setup_preview();
        }

        ImNodes::SetCurrentContext(prev_imnodes);
    }
    ImGui::End();
}

// ── Module Stack ─────────────────────────────────────────────────────────

void VfxEditorContext::draw_module_entry(VfxModule* mod, int index,
                                          std::vector<std::unique_ptr<VfxModule>>& stack,
                                          const char* /*stack_label*/) {
    ImGui::PushID(index);

    ImGui::Checkbox("##en", &mod->enabled);
    ImGui::SameLine();

    bool open = ImGui::TreeNodeEx(mod->name(),
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);

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
    if (ImGui::Begin(ctx_title("Module Stack").c_str())) {
        if (!effect_ || selected_emitter_ < 0 ||
            selected_emitter_ >= static_cast<int>(effect_->emitters.size())) {
            ImGui::TextDisabled("Select an emitter node");
            ImGui::End();
            return;
        }

        auto& emitter = effect_->emitters[selected_emitter_];

        // Emitter settings
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

        ImGui::SeparatorText("Spawn");
        for (int i = 0; i < static_cast<int>(emitter.spawn_modules.size()); ++i) {
            draw_module_entry(emitter.spawn_modules[i].get(), i,
                              emitter.spawn_modules, "spawn");
        }

        ImGui::SeparatorText("Update");
        for (int i = 0; i < static_cast<int>(emitter.update_modules.size()); ++i) {
            draw_module_entry(emitter.update_modules[i].get(), i,
                              emitter.update_modules, "update");
        }

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
    if (ImGui::Begin(ctx_title("Add Module").c_str())) {
        if (!effect_ || selected_emitter_ < 0 ||
            selected_emitter_ >= static_cast<int>(effect_->emitters.size())) {
            ImGui::TextDisabled("Select an emitter node");
            ImGui::End();
            return;
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
