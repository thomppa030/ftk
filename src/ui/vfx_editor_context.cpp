#include "ui/vfx_editor_context.hpp"
#include "ui/dockspace_layout.hpp"
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
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "ui/icon_cache.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_node_editor.h>

#include <cstdio>
#include <filesystem>

namespace ne = ax::NodeEditor;

namespace fjell {

namespace fs = std::filesystem;

// ── Pin colors by type ──────────────────────────────────────────────────

static ImColor pin_color(PinDataType type) {
    switch (type) {
        case PinDataType::float_type: return ImColor(126, 191, 142);  // green
        case PinDataType::vec3_type:  return ImColor(100, 148, 220);  // blue
        case PinDataType::vec4_type:  return ImColor(180, 100, 220);  // purple
        case PinDataType::uint_type:  return ImColor(100, 204, 220);  // cyan
        case PinDataType::enum_type:  return ImColor(212, 160, 84);   // gold
    }
    return ImColor(180, 180, 180);
}

// ── Stack colors ────────────────────────────────────────────────────────

static constexpr ImU32 kSpawnColor  = IM_COL32(80, 160, 90, 255);
static constexpr ImU32 kUpdateColor = IM_COL32(70, 130, 200, 255);
static constexpr ImU32 kRenderColor = IM_COL32(210, 140, 60, 255);

static ImU32 stack_color(int stack) {
    switch (stack) {
        case 0: return kSpawnColor;
        case 1: return kUpdateColor;
        case 2: return kRenderColor;
    }
    return IM_COL32(140, 140, 140, 255);
}

// ── Node/Pin ID encoding ────────────────────────────────────────────────

int VfxEditorContext::module_node_id(int stack, int index) {
    int base = (stack == 0) ? kSpawnBase : (stack == 1) ? kUpdateBase : kRenderBase;
    return base + index;
}

int VfxEditorContext::input_pin_id(int node_id, int pin_index) {
    return node_id * 100 + pin_index;
}

int VfxEditorContext::output_pin_id(int node_id) {
    return node_id * 100 + 50;
}

void VfxEditorContext::decode_node_id(int node_id, int& stack, int& index) const {
    if (node_id >= kRenderBase) {
        stack = 2;
        index = node_id - kRenderBase;
    } else if (node_id >= kUpdateBase) {
        stack = 1;
        index = node_id - kUpdateBase;
    } else {
        stack = 0;
        index = node_id - kSpawnBase;
    }
}

// ── VfxTimeline ─────────────────────────────────────────────────────────

void VfxTimeline::draw_lanes(float bar_x, float bar_y, float bar_width,
                              float lane_height) {
    TimelineWidget::draw_lanes(bar_x, bar_y, bar_width, lane_height);

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
                           deps_.vulkan_context->allocator(),
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
        for (auto& em : effect_->emitters) {
            em.burst_fired = false;
            em.emit_accumulator = 0.0f;
            em.paused = false;
        }
    }));

    // Create imgui-node-editor context
    ne::Config config;
    config.SettingsFile = nullptr; // in-memory only
    node_editor_ctx_ = ne::CreateEditor(&config);

    // Style: thicker links, more visible
    ne::SetCurrentEditor(node_editor_ctx_);
    auto& style = ne::GetStyle();
    style.LinkStrength = 200.0f;
    style.ScrollDuration = 0.35f;
    style.NodeRounding = 6.0f;
    style.NodeBorderWidth = 1.5f;
    style.HoveredNodeBorderWidth = 2.5f;
    style.SelectedNodeBorderWidth = 2.5f;
    style.PinRounding = 3.0f;
    ne::SetCurrentEditor(nullptr);

    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

void VfxEditorContext::shutdown() {
    timeline_connections_.clear();
    if (node_editor_ctx_) {
        ne::DestroyEditor(node_editor_ctx_);
        node_editor_ctx_ = nullptr;
    }
    viewport_manager_.release_textures();
}

void VfxEditorContext::draw(float dt) {
    if (auto* vp = viewport_manager_.active_panel()) {
        auto t = ctx_title("VFX Preview");
        if (vp->title != t) vp->title = std::move(t);
    }
    timeline_.set_title(ctx_title("VFX Timeline"));
    preview_scene_.compute_world_matrices(nullptr);
    viewport_manager_.draw(*deps_.vulkan_context, *deps_.input, dt);
    draw_node_editor();
    draw_module_stack();

    // Build timeline lanes from emitters
    if (effect_) {
        static const uint32_t lane_colors[] = {
            IM_COL32(220, 120, 50, 180),
            IM_COL32(100, 180, 220, 180),
            IM_COL32(140, 140, 140, 180),
            IM_COL32(180, 100, 220, 180),
            IM_COL32(100, 220, 120, 180),
            IM_COL32(220, 200, 80, 180),
        };
        std::vector<TimelineLane> lanes;
        for (int i = 0; i < static_cast<int>(effect_->emitters.size()); ++i) {
            auto& em = effect_->emitters[i];
            TimelineLane lane;
            lane.name = em.name;
            lane.start = 0.0f;
            lane.duration = em.lifetime_max();
            lane.color = lane_colors[i % 6];
            lane.selected = (i == selected_emitter_);
            lanes.push_back(lane);
        }
        timeline_.set_lanes(lanes);
    }

    timeline_.draw(dt * time_scale_);

    float cur_time = timeline_.time();
    if (effect_ && timeline_.playing() && cur_time < prev_timeline_time_) {
        for (auto& em : effect_->emitters) {
            em.burst_fired = false;
            em.emit_accumulator = 0.0f;
        }
    }
    prev_timeline_time_ = cur_time;

    draw_shared_panels(dt);
}

std::vector<std::string> VfxEditorContext::docked_window_names() const {
    return {ctx_title("VFX Preview"), ctx_title("Module Stack"),
            ctx_title("VFX Graph"), ctx_title("VFX Timeline"),
            ctx_title("Stats"), ctx_title("Console"), ctx_title("History")};
}

void VfxEditorContext::setup_dockspace(ImGuiID main_area) {
    enum class EPanel { Preview, ModuleStack, Graph, Timeline, Stats, Console, History };
    using N = DockNode;
    // Graph gets the big center area, module stack on the right
    static const auto layout = N::horizontal_split(0.72f,
        N::vertical_split(0.55f,
            N::leaf({EPanel::Preview}),
            N::vertical_split(0.65f,
                N::leaf({EPanel::Graph}),
                N::leaf({EPanel::Timeline, EPanel::Stats, EPanel::Console, EPanel::History}))),
        N::leaf({EPanel::ModuleStack}));
    build_dockspace(main_area, layout, docked_window_names());
}

bool VfxEditorContext::handle_shortcuts() {
    if (ImGui::GetIO().WantTextInput) return false;

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
    return viewport_manager_.build_render_requests();
}

void VfxEditorContext::apply_resize(VulkanContext& vk) {
    viewport_manager_.apply_resize(vk);
}

bool VfxEditorContext::any_viewport_hovered() const {
    return viewport_manager_.any_hovered();
}

// ── Open / Save ─────────────────────────────────────────────────────────

void VfxEditorContext::open_effect(const std::string& fjvfx_path) {
    fjvfx_path_ = fjvfx_path;
    fjvfx_abs_path_ = base_path::resolve_asset(fjvfx_path, deps_.project_mgr->active());

    try {
        auto loaded = load_vfx_effect(fjvfx_abs_path_);
        effect_ = std::make_shared<VfxEffect>(std::move(loaded));

        // Resolve texture paths to bindless indices
        for (auto& emitter : effect_->emitters) {
            for (auto& mod : emitter.spawn_modules) {
                if (mod->type() == ModuleType::initial_texture) {
                    auto* tex_mod = static_cast<InitialTextureModule*>(mod.get());
                    if (!tex_mod->texture_path.empty() && tex_mod->bindless_index == 0) {
                        auto resolved = base_path::resolve_asset(
                            tex_mod->texture_path, deps_.project_mgr->active());
                        auto result = deps_.resource_manager->load_texture(resolved);
                        if (result) {
                            tex_mod->bindless_index = deps_.resource_manager->bindless_index(*result);
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("VFX editor: failed to load {}: {}", fjvfx_path, e.what());
        return;
    }

    name_ = fs::path(fjvfx_path).stem().string();
    selected_emitter_ = effect_->emitters.empty() ? -1 : 0;
    selected_module_stack_ = -1;
    selected_module_index_ = -1;
    needs_initial_layout_ = true;

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

// ── Node layout helper ──────────────────────────────────────────────────

void VfxEditorContext::rebuild_node_positions() {
    if (!effect_ || selected_emitter_ < 0) return;

    auto& emitter = effect_->emitters[selected_emitter_];

    float x = 50.0f;
    float y = 30.0f;
    float y_spacing = 140.0f;

    // Spawn stack
    for (int i = 0; i < static_cast<int>(emitter.spawn_modules.size()); ++i) {
        ne::SetNodePosition(ne::NodeId(module_node_id(0, i)), ImVec2(x, y));
        y += y_spacing;
    }

    // Update stack — offset right
    y = 30.0f;
    x = 400.0f;
    for (int i = 0; i < static_cast<int>(emitter.update_modules.size()); ++i) {
        ne::SetNodePosition(ne::NodeId(module_node_id(1, i)), ImVec2(x, y));
        y += y_spacing;
    }

    // Render stack — offset further right
    y = 30.0f;
    x = 750.0f;
    for (int i = 0; i < static_cast<int>(emitter.render_modules.size()); ++i) {
        ne::SetNodePosition(ne::NodeId(module_node_id(2, i)), ImVec2(x, y));
        y += y_spacing;
    }
}

// ── Node Editor ─────────────────────────────────────────────────────────

static constexpr float kPinIconSize = 12.0f;

static void draw_pin_icon(PinDataType type, bool connected, float size = kPinIconSize) {
    ImVec2 sz(size, size);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 center(pos.x + sz.x * 0.5f, pos.y + sz.y * 0.5f);
    float r = sz.x * 0.4f;
    ImColor col = pin_color(type);
    if (connected) {
        dl->AddCircleFilled(center, r, col);
    } else {
        dl->AddCircle(center, r, col, 12, 2.0f);
    }
    ImGui::Dummy(sz);
}

/// Draw a colored separator line at a fixed width.
static void draw_colored_separator(ImU32 color, float width = 180.0f) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(
        p, ImVec2(p.x + width, p.y), color, 1.0f);
    ImGui::Dummy(ImVec2(width, 2));
}

void VfxEditorContext::draw_node_editor() {
    if (ImGui::Begin(ctx_title("VFX Graph").c_str())) {
        if (!effect_ || !node_editor_ctx_) {
            ImGui::TextDisabled("No effect loaded");
            ImGui::End();
            return;
        }

        // Emitter selector + toolbar
        {
            // Emitter dropdown
            if (ImGui::BeginCombo("##emitter",
                    selected_emitter_ >= 0
                        ? effect_->emitters[selected_emitter_].name.c_str()
                        : "Select emitter")) {
                for (int i = 0; i < static_cast<int>(effect_->emitters.size()); ++i) {
                    bool sel = (i == selected_emitter_);
                    if (ImGui::Selectable(effect_->emitters[i].name.c_str(), sel)) {
                        selected_emitter_ = i;
                        selected_module_stack_ = -1;
                        selected_module_index_ = -1;
                        needs_initial_layout_ = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+ Emitter")) {
                effect_->emitters.push_back(VfxEmitter::make_default());
                auto& em = effect_->emitters.back();
                em.name = "emitter_" + std::to_string(effect_->emitters.size());
                selected_emitter_ = static_cast<int>(effect_->emitters.size()) - 1;
                needs_initial_layout_ = true;
                setup_preview();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Save")) {
                save_effect();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderFloat("Speed", &time_scale_, 0.0f, 3.0f, "%.1f");
        }

        if (selected_emitter_ < 0 ||
            selected_emitter_ >= static_cast<int>(effect_->emitters.size())) {
            ImGui::TextDisabled("No emitter selected");
            ImGui::End();
            return;
        }

        auto& emitter = effect_->emitters[selected_emitter_];

        ne::SetCurrentEditor(node_editor_ctx_);
        ne::Begin("VFX Module Graph");

        if (needs_initial_layout_) {
            rebuild_node_positions();
            needs_initial_layout_ = false;
            ne::NavigateToContent(0.0f);
        }

        // Value preview — visible light color, not disabled gray
        static const ImVec4 kValueColor(0.65f, 0.68f, 0.72f, 1.0f);
        auto draw_value_preview = [](const PinDescriptor& pin) {
            switch (pin.data_type) {
                case PinDataType::float_type: {
                    float v = *static_cast<float*>(pin.value_ptr);
                    ImGui::TextColored(kValueColor, "%.2f", v);
                    break;
                }
                case PinDataType::vec3_type: {
                    auto v = *static_cast<glm::vec3*>(pin.value_ptr);
                    ImGui::TextColored(kValueColor, "(%.1f, %.1f, %.1f)", v.x, v.y, v.z);
                    break;
                }
                case PinDataType::vec4_type: {
                    auto v = *static_cast<glm::vec4*>(pin.value_ptr);
                    ImVec4 c(v.r, v.g, v.b, v.a);
                    ImGui::ColorButton("##col", c,
                        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                        ImVec2(14, 14));
                    break;
                }
                case PinDataType::uint_type: {
                    uint32_t v = *static_cast<uint32_t*>(pin.value_ptr);
                    ImGui::TextColored(kValueColor, "%u", v);
                    break;
                }
                case PinDataType::enum_type: {
                    uint32_t v = *static_cast<uint32_t*>(pin.value_ptr);
                    if (v < pin.enum_names.size()) {
                        ImGui::TextColored(kValueColor, "%s", pin.enum_names[v].c_str());
                    } else {
                        ImGui::TextColored(kValueColor, "%u", v);
                    }
                    break;
                }
            }
        };

        // Draw module nodes for each stack
        auto draw_stack = [&](int stack_idx, auto& modules) {
            for (int i = 0; i < static_cast<int>(modules.size()); ++i) {
                auto* mod = modules[i].get();
                int nid = module_node_id(stack_idx, i);

                bool is_selected = (stack_idx == selected_module_stack_ &&
                                    i == selected_module_index_);

                ImU32 col = stack_color(stack_idx);
                ne::PushStyleColor(ne::StyleColor_NodeBg, ImColor(32, 33, 36, 230));
                ne::PushStyleColor(ne::StyleColor_NodeBorder,
                    is_selected ? ImColor(212, 160, 84, 255) : ImColor(55, 57, 62, 255));
                ne::PushStyleVar(ne::StyleVar_NodeRounding, 6.0f);
                ne::PushStyleVar(ne::StyleVar_NodePadding, ImVec4(8, 6, 8, 8));

                ne::BeginNode(ne::NodeId(nid));

                // ── Title ──
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::Text("%s", mod->name());
                ImGui::PopStyleColor();
                if (!mod->enabled) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(off)");
                }
                draw_colored_separator(col);

                // ── Input pins (left) ──
                auto pins = mod->pin_descriptors();
                for (int p = 0; p < static_cast<int>(pins.size()); ++p) {
                    if (pins[p].data_type == PinDataType::enum_type) continue;

                    ne::BeginPin(ne::PinId(input_pin_id(nid, p)), ne::PinKind::Input);
                        draw_pin_icon(pins[p].data_type, false);
                    ne::EndPin();
                    ImGui::SameLine();
                    ImGui::Text("%s", pins[p].name.c_str());
                    ImGui::SameLine();
                    draw_value_preview(pins[p]);
                }

                // ── Single output pin (right, own row) ──
                ImGui::Text("out");
                ImGui::SameLine();
                ne::BeginPin(ne::PinId(output_pin_id(nid)), ne::PinKind::Output);
                    draw_pin_icon(PinDataType::float_type, false);
                ne::EndPin();

                ne::EndNode();
                ne::PopStyleVar(2);
                ne::PopStyleColor(2);
            }
        };

        draw_stack(0, emitter.spawn_modules);
        draw_stack(1, emitter.update_modules);
        draw_stack(2, emitter.render_modules);

        // Draw expression nodes
        draw_expression_nodes();

        // Draw links
        draw_links();

        // Handle link creation
        handle_link_creation();

        // Handle deletion
        handle_deletion();

        // Context menu (right-click on canvas)
        ne::Suspend();
        if (ne::ShowBackgroundContextMenu()) {
            ImGui::OpenPopup("##add_module");
        }
        draw_context_menu();
        ne::Resume();

        ne::End();

        // Handle node selection → sync to module stack panel
        if (ne::HasSelectionChanged()) {
            ne::NodeId sel_nodes[1];
            int count = ne::GetSelectedNodes(sel_nodes, 1);
            if (count > 0) {
                int nid = static_cast<int>(sel_nodes[0].Get());
                // Only select module nodes, not expression nodes
                if (nid >= kSpawnBase && nid < 50000) {
                    int stack, index;
                    decode_node_id(nid, stack, index);
                    selected_module_stack_ = stack;
                    selected_module_index_ = index;
                }
            }
        }

        ne::SetCurrentEditor(nullptr);
    }
    ImGui::End();
}

// ── Context Menu (replaces Add Module panel) ────────────────────────────

void VfxEditorContext::draw_context_menu() {
    if (!effect_ || selected_emitter_ < 0) return;
    auto& emitter = effect_->emitters[selected_emitter_];

    if (ImGui::BeginPopup("##add_module")) {
        auto add_btn = [&](const char* label, auto make_fn, auto& stack, int stack_idx) {
            if (ImGui::MenuItem(label)) {
                stack.push_back(make_fn());
                needs_initial_layout_ = true;
                selected_module_stack_ = stack_idx;
                selected_module_index_ = static_cast<int>(stack.size()) - 1;
            }
        };

        if (ImGui::BeginMenu("Spawn")) {
            add_btn("Spawn Rate",       []{ return std::make_unique<SpawnRateModule>(); },        emitter.spawn_modules, 0);
            add_btn("Initial Lifetime", []{ return std::make_unique<InitialLifetimeModule>(); },  emitter.spawn_modules, 0);
            add_btn("Initial Position", []{ return std::make_unique<InitialPositionModule>(); },  emitter.spawn_modules, 0);
            add_btn("Initial Velocity", []{ return std::make_unique<InitialVelocityModule>(); },  emitter.spawn_modules, 0);
            add_btn("Initial Color",    []{ return std::make_unique<InitialColorModule>(); },     emitter.spawn_modules, 0);
            add_btn("Initial Size",     []{ return std::make_unique<InitialSizeModule>(); },      emitter.spawn_modules, 0);
            add_btn("Burst",            []{ return std::make_unique<BurstModule>(); },            emitter.spawn_modules, 0);
            add_btn("Initial Rotation", []{ return std::make_unique<InitialRotationModule>(); },  emitter.spawn_modules, 0);
            add_btn("Initial Texture",  []{ return std::make_unique<InitialTextureModule>(); },   emitter.spawn_modules, 0);
            add_btn("Flipbook",         []{ return std::make_unique<FlipbookModule>(); },         emitter.spawn_modules, 0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Update")) {
            add_btn("Gravity",          []{ return std::make_unique<GravityModule>(); },          emitter.update_modules, 1);
            add_btn("Color Over Life",  []{ return std::make_unique<ColorOverLifeModule>(); },    emitter.update_modules, 1);
            add_btn("Size Over Life",   []{ return std::make_unique<SizeOverLifeModule>(); },     emitter.update_modules, 1);
            add_btn("Drag",             []{ return std::make_unique<DragModule>(); },             emitter.update_modules, 1);
            add_btn("Noise",            []{ return std::make_unique<NoiseModule>(); },            emitter.update_modules, 1);
            add_btn("Attract",          []{ return std::make_unique<AttractModule>(); },          emitter.update_modules, 1);
            add_btn("Velocity Limit",   []{ return std::make_unique<VelocityLimitModule>(); },   emitter.update_modules, 1);
            add_btn("Collision",        []{ return std::make_unique<CollisionModule>(); },        emitter.update_modules, 1);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Render")) {
            add_btn("Billboard",        []{ return std::make_unique<BillboardModule>(); },        emitter.render_modules, 2);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Expression")) {
            auto& graph = emitter.expression_graph;
            auto add_expr = [&](const char* label, ExprNodeType type) {
                if (ImGui::MenuItem(label)) {
                    graph.add_node(type);
                }
            };

            if (ImGui::BeginMenu("Constants")) {
                add_expr("Float",  ExprNodeType::constant_float);
                add_expr("Vec3",   ExprNodeType::constant_vec3);
                add_expr("Color",  ExprNodeType::constant_vec4);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Sources")) {
                add_expr("Time",       ExprNodeType::time);
                add_expr("Delta Time", ExprNodeType::delta_time);
                add_expr("Random",     ExprNodeType::random);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Math")) {
                add_expr("Add",      ExprNodeType::add);
                add_expr("Subtract", ExprNodeType::subtract);
                add_expr("Multiply", ExprNodeType::multiply);
                add_expr("Divide",   ExprNodeType::divide);
                add_expr("Negate",   ExprNodeType::negate);
                add_expr("Abs",      ExprNodeType::abs_node);
                add_expr("Sin",      ExprNodeType::sin_node);
                add_expr("Cos",      ExprNodeType::cos_node);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Interpolation")) {
                add_expr("Lerp",   ExprNodeType::lerp);
                add_expr("Remap",  ExprNodeType::remap);
                add_expr("Clamp",  ExprNodeType::clamp);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Conversion")) {
                add_expr("Split Vec3",   ExprNodeType::split_vec3);
                add_expr("Combine Vec3", ExprNodeType::combine_vec3);
                add_expr("Split Vec4",   ExprNodeType::split_vec4);
                add_expr("Combine Vec4", ExprNodeType::combine_vec4);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
}

// ── Module Stack (inspector for selected node) ─────────────────────────

void VfxEditorContext::draw_module_entry(VfxModule* mod, int index,
                                          std::vector<std::unique_ptr<VfxModule>>& stack,
                                          const char* stack_label) {
    ImGui::PushID(stack_label);
    ImGui::PushID(index);

    float btn_w = ImGui::CalcTextSize("X").x + ImGui::GetStyle().FramePadding.x * 2;
    float btn_x = ImGui::GetWindowWidth() - btn_w - 10;

    ImGui::Checkbox("##en", &mod->enabled);
    ImGui::SameLine();

    float avail = btn_x - ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x;
    ImGui::PushItemWidth(avail);
    bool open = ImGui::TreeNodeEx(mod->name(), ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopItemWidth();

    ImGui::SameLine(btn_x);
    if (ImGui::SmallButton("X")) {
        stack.erase(stack.begin() + index);
        needs_initial_layout_ = true;
        if (open) ImGui::TreePop();
        ImGui::PopID();
        ImGui::PopID();
        return;
    }

    if (open) {
        if (mod->enabled) {
            mod->draw_inspector();

            // Texture module: thumbnail + drag-drop
            if (mod->type() == ModuleType::initial_texture) {
                auto* tex_mod = static_cast<InitialTextureModule*>(mod);

                float thumb_size = 48.0f;
                if (!tex_mod->texture_path.empty() && deps_.icon_cache) {
                    auto resolved = base_path::resolve_asset(
                        tex_mod->texture_path, deps_.project_mgr->active());
                    auto desc = deps_.icon_cache->thumbnail(resolved);
                    if (desc) {
                        ImGui::Image(desc, {thumb_size, thumb_size});
                    } else {
                        ImGui::Dummy({thumb_size, thumb_size});
                    }
                } else {
                    ImGui::Dummy({thumb_size, thumb_size});
                }

                if (!tex_mod->texture_path.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear")) {
                        tex_mod->texture_path.clear();
                        tex_mod->bindless_index = 0;
                    }
                }

                if (ImGui::BeginDragDropTarget()) {
                    if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        std::string path(static_cast<const char*>(payload->Data));
                        auto ext = std::filesystem::path(path).extension().string();
                        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
                            tex_mod->texture_path = path;
                            auto result = deps_.resource_manager->load_texture(path);
                            if (result) {
                                tex_mod->bindless_index = deps_.resource_manager->bindless_index(*result);
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
            }
        } else {
            ImGui::TextDisabled("(disabled)");
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
    ImGui::PopID();
}

void VfxEditorContext::draw_module_stack() {
    if (ImGui::Begin(ctx_title("Module Stack").c_str())) {
        if (!effect_ || selected_emitter_ < 0 ||
            selected_emitter_ >= static_cast<int>(effect_->emitters.size())) {
            ImGui::TextDisabled("No emitter selected");
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

        // If a specific module is selected, show focused view
        if (selected_module_stack_ >= 0 && selected_module_index_ >= 0) {
            auto* stack_ptr =
                (selected_module_stack_ == 0) ? &emitter.spawn_modules :
                (selected_module_stack_ == 1) ? &emitter.update_modules :
                                                &emitter.render_modules;
            if (selected_module_index_ < static_cast<int>(stack_ptr->size())) {
                const char* stack_names[] = {"Spawn", "Update", "Render"};
                ImGui::TextColored(ImColor(stack_color(selected_module_stack_)).Value,
                                   "%s", stack_names[selected_module_stack_]);
                ImGui::SameLine();
                ImGui::TextDisabled("(%d/%d)",
                    selected_module_index_ + 1,
                    static_cast<int>(stack_ptr->size()));
                ImGui::Separator();

                draw_module_entry((*stack_ptr)[selected_module_index_].get(),
                                  selected_module_index_, *stack_ptr,
                                  stack_names[selected_module_stack_]);
                ImGui::End();
                return;
            }
        }

        // Fallback: show all stacks
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

// ── Expression Nodes ────────────────────────────────────────────────────

void VfxEditorContext::draw_expression_nodes() {
    if (!effect_ || selected_emitter_ < 0) return;
    auto& graph = effect_->emitters[selected_emitter_].expression_graph;

    for (auto& node : graph.nodes) {
        auto nid = ne::NodeId(node->node_id);
        auto pins = expr_node_pins(node->type);

        ne::PushStyleColor(ne::StyleColor_NodeBg, ImColor(28, 30, 34, 230));
        ne::PushStyleColor(ne::StyleColor_NodeBorder, ImColor(65, 70, 80, 255));
        ne::PushStyleVar(ne::StyleVar_NodeRounding, 4.0f);
        ne::PushStyleVar(ne::StyleVar_NodePadding, ImVec4(8, 4, 8, 6));

        ne::BeginNode(nid);

        // ── Title ──
        ImGui::TextColored(ImVec4(0.6f, 0.65f, 0.75f, 1.0f),
                           "%s", expr_node_name(node->type));
        draw_colored_separator(IM_COL32(80, 88, 100, 200), 100.0f);

        // ── Constant value editing ──
        if (node->type == ExprNodeType::constant_float) {
            float v = std::get<float>(node->constant_value);
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::DragFloat("##val", &v, 0.01f)) {
                node->constant_value = v;
            }
        } else if (node->type == ExprNodeType::constant_vec3) {
            auto v = std::get<glm::vec3>(node->constant_value);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::DragFloat3("##val", &v.x, 0.01f)) {
                node->constant_value = v;
            }
        } else if (node->type == ExprNodeType::constant_vec4) {
            auto v = std::get<glm::vec4>(node->constant_value);
            if (ImGui::ColorEdit4("##val", &v.x,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                node->constant_value = v;
            }
        }

        // ── Collect input and output pin indices ──
        std::vector<int> in_idx, out_idx;
        for (int p = 0; p < static_cast<int>(pins.size()); ++p) {
            if (pins[p].is_output) out_idx.push_back(p);
            else in_idx.push_back(p);
        }

        // Pair inputs and outputs on same rows
        int rows = std::max(static_cast<int>(in_idx.size()),
                            static_cast<int>(out_idx.size()));
        for (int r = 0; r < rows; ++r) {
            // Input on left
            if (r < static_cast<int>(in_idx.size())) {
                int p = in_idx[r];
                ne::BeginPin(ne::PinId(node->node_id * 100 + p), ne::PinKind::Input);
                    draw_pin_icon(pins[p].type, false);
                ne::EndPin();
                ImGui::SameLine();
                ImGui::Text("%s", pins[p].name);
            }

            // Output on right (same line)
            if (r < static_cast<int>(out_idx.size())) {
                int p = out_idx[r];
                ImGui::SameLine(0, 16.0f);
                ImGui::Text("%s", pins[p].name);
                ImGui::SameLine();
                ne::BeginPin(ne::PinId(node->node_id * 100 + 50 + p), ne::PinKind::Output);
                    draw_pin_icon(pins[p].type, false);
                ne::EndPin();
            }
        }

        ne::EndNode();
        ne::PopStyleVar(2);
        ne::PopStyleColor(2);
    }
}

// ── Links ───────────────────────────────────────────────────────────────

void VfxEditorContext::draw_links() {
    if (!effect_ || selected_emitter_ < 0) return;
    auto& graph = effect_->emitters[selected_emitter_].expression_graph;

    for (auto& link : graph.links) {
        // Determine color from source pin type
        PinDataType src_type = resolve_pin_type(link.source_pin_id);
        ne::Link(ne::LinkId(link.link_id),
                 ne::PinId(link.source_pin_id),
                 ne::PinId(link.target_pin_id),
                 ImColor(pin_color(src_type)), 3.0f);
    }
}

// ── Link Creation ───────────────────────────────────────────────────────

static bool types_compatible(PinDataType source, PinDataType target) {
    if (source == target) return true;
    // Float can broadcast to vec3/vec4
    if (source == PinDataType::float_type &&
        (target == PinDataType::vec3_type || target == PinDataType::vec4_type)) {
        return true;
    }
    return false;
}

void VfxEditorContext::handle_link_creation() {
    if (!effect_ || selected_emitter_ < 0) return;
    auto& graph = effect_->emitters[selected_emitter_].expression_graph;

    if (ne::BeginCreate()) {
        ne::PinId start_id, end_id;
        if (ne::QueryNewLink(&start_id, &end_id)) {
            if (start_id && end_id) {
                auto src = static_cast<uint64_t>(start_id.Get());
                auto dst = static_cast<uint64_t>(end_id.Get());

                PinDataType src_type = resolve_pin_type(src);
                PinDataType dst_type = resolve_pin_type(dst);

                if (types_compatible(src_type, dst_type)) {
                    if (ne::AcceptNewItem(ImColor(pin_color(src_type)), 3.0f)) {
                        graph.add_link(src, dst);
                    }
                } else {
                    ne::RejectNewItem(ImColor(200, 50, 50, 255), 2.0f);
                }
            }
        }
    }
    ne::EndCreate();
}

// ── Deletion ────────────────────────────────────────────────────────────

void VfxEditorContext::handle_deletion() {
    if (!effect_ || selected_emitter_ < 0) return;
    auto& graph = effect_->emitters[selected_emitter_].expression_graph;

    if (ne::BeginDelete()) {
        ne::LinkId lid;
        while (ne::QueryDeletedLink(&lid)) {
            if (ne::AcceptDeletedItem()) {
                graph.remove_link(static_cast<uint64_t>(lid.Get()));
            }
        }
        ne::NodeId nid;
        while (ne::QueryDeletedNode(&nid)) {
            auto id = static_cast<uint64_t>(nid.Get());
            // Only allow deleting expression nodes (id >= 50000)
            if (id >= 50000) {
                if (ne::AcceptDeletedItem()) {
                    graph.remove_node(id);
                }
            } else {
                ne::RejectDeletedItem();
            }
        }
    }
    ne::EndDelete();
}

// ── Pin type resolution ─────────────────────────────────────────────────

PinDataType VfxEditorContext::resolve_pin_type(uint64_t pin_id) const {
    if (!effect_ || selected_emitter_ < 0) return PinDataType::float_type;
    auto& emitter = effect_->emitters[selected_emitter_];

    // Expression node pin: node_id * 100 + offset
    uint64_t node_id = pin_id / 100;
    int pin_idx = static_cast<int>(pin_id % 100);

    // Check expression nodes
    if (node_id >= 50000) {
        auto* node = emitter.expression_graph.find_node(node_id);
        if (node) {
            auto pins = expr_node_pins(node->type);
            // Map pin_idx: outputs start at 50
            if (pin_idx >= 50) {
                int out_idx = 0;
                for (auto& pd : pins) {
                    if (pd.is_output) {
                        if (out_idx == pin_idx - 50) return pd.type;
                        ++out_idx;
                    }
                }
            } else {
                int in_idx = 0;
                for (auto& pd : pins) {
                    if (!pd.is_output) {
                        if (in_idx == pin_idx) return pd.type;
                        ++in_idx;
                    }
                }
            }
        }
        return PinDataType::float_type;
    }

    // Module node pin
    int nid = static_cast<int>(node_id);
    if (pin_idx >= 50) {
        // Output pin — modules have a generic "out" pin, treat as float
        return PinDataType::float_type;
    }

    int stack, mod_idx;
    decode_node_id(nid, stack, mod_idx);

    auto* stack_ptr =
        (stack == 0) ? &emitter.spawn_modules :
        (stack == 1) ? &emitter.update_modules :
                       &emitter.render_modules;

    if (mod_idx >= 0 && mod_idx < static_cast<int>(stack_ptr->size())) {
        auto pins = (*stack_ptr)[mod_idx]->pin_descriptors();
        if (pin_idx >= 0 && pin_idx < static_cast<int>(pins.size())) {
            return pins[pin_idx].data_type;
        }
    }

    return PinDataType::float_type;
}

} // namespace fjell
