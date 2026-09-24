#include "ui/vfx_editor_context.hpp"
#include "ui/inspectors/vfx_module_inspectors.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/component_block.hpp"
#include "ui/kit/dialog.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/menu.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/kit/tabs.hpp"
#include "ui/kit/text_field.hpp"
#include "ui/panel_widget.hpp"
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
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "core/string_utils.hpp"
#include "ui/icon_cache.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <optional>
#include <imgui.h>
#include <nlohmann/json.hpp>
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

VfxEditorContext::VfxEditorContext(VfxEditorDeps deps)
    : deps_{deps} {}

EditorContext::DockPreset VfxEditorContext::dock_preset() const {
    return {.preview = {"Preview"}, .under_preview = {"Graph", "Timeline"}, .inspector = {"Modules"}};
}

void VfxEditorContext::init() {
    viewport_manager_.init(deps_.vulkan_context->device_ref(),
                           deps_.vulkan_context->allocator(),
                           deps_.vulkan_context->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Preview");

    timeline_.set_title("Timeline");
    timeline_.set_category(theme::Category::Vfx);
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

    // Create imgui-node-editor context
    ne::Config config;
    config.SettingsFile = nullptr; // in-memory only
    node_editor_ctx_ = ne::CreateEditor(&config);

    // Style
    ne::SetCurrentEditor(node_editor_ctx_);
    auto& style = ne::GetStyle();
    style.LinkStrength = 200.0f;
    style.ScrollDuration = 0.35f;
    style.NodeRounding = 4.0f;
    style.NodeBorderWidth = 1.0f;
    style.HoveredNodeBorderWidth = 1.5f;
    style.SelectedNodeBorderWidth = 2.0f;
    style.PinRounding = 3.0f;
    style.Colors[ne::StyleColor_Bg]            = ImColor(22, 23, 26, 255);
    style.Colors[ne::StyleColor_Grid]          = ImColor(38, 40, 44, 128);
    style.Colors[ne::StyleColor_NodeBg]        = ImColor(30, 31, 34, 240);
    style.Colors[ne::StyleColor_NodeBorder]    = ImColor(52, 54, 58, 255);
    style.Colors[ne::StyleColor_HovNodeBorder] = ImColor(80, 85, 95, 255);
    style.Colors[ne::StyleColor_SelNodeBorder] = ImColor(212, 160, 84, 255);
    ne::SetCurrentEditor(nullptr);

    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

void VfxEditorContext::shutdown() {
    timeline_connections_.clear();
    deps_.vulkan_context->release_scene_particles(preview_scene_);
    if (node_editor_ctx_) {
        ne::DestroyEditor(node_editor_ctx_);
        node_editor_ctx_ = nullptr;
    }
    viewport_manager_.release_textures();
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
    draw_node_editor();
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
        FJELL_CORE_ERROR("VFX editor: failed to load {}: {}", fjvfx_path, e.what());
        return;
    }

    name_ = fs::path(fjvfx_path).stem().string();
    selected_emitter_ = effect_->emitters.empty() ? -1 : 0;
    selected_module_stack_ = -1;
    selected_module_index_ = -1;
    needs_initial_layout_ = true;

    timeline_.set_duration(std::max(effect_->max_lifetime() + 1.0f, 3.0f));
    timeline_.show(fjvfx_path);

    setup_preview();

    viewport_manager_.frame({0.0f, 1.0f, 0.0f}, 3.0f);

    document_opened();
    FJELL_CORE_INFO("VFX editor opened: {}", fjvfx_path);

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
        FJELL_CORE_ERROR("VFX hot-reload failed for {}: {}", fjvfx_path_, e.what());
        return;
    }
    document_opened();
    FJELL_CORE_INFO("VFX hot-reloaded: {}", fjvfx_path_);
}

#endif

Result<> VfxEditorContext::restore(const std::string& text) {
    try {
        take_effect(load_vfx_effect_from_json(nlohmann::json::parse(text)));
    } catch (const std::exception& e) {
        return make_error(e.what());
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
    // Node positions reach the effect only when saving, so moving a node
    // alone isn't an unsaved change.
    return effect_ ? vfx_effect_text(*effect_) : std::string{};
}

Result<> VfxEditorContext::write() {
    if (fjvfx_abs_path_.empty() || !effect_) return make_error("no effect is open");

    // Sync current emitter's node positions before saving
    if (node_editor_ctx_) {
        ne::SetCurrentEditor(node_editor_ctx_);
        sync_node_positions();
        ne::SetCurrentEditor(nullptr);
    }

    try {
        save_vfx_effect(fjvfx_abs_path_, *effect_);
    } catch (const std::exception& e) {
        return make_error(e.what());
    }
    FJELL_CORE_INFO("VFX saved: {}", fjvfx_abs_path_);
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

// ── Node layout helper ──────────────────────────────────────────────────

void VfxEditorContext::rebuild_node_positions() {
    if (!effect_ || selected_emitter_ < 0) return;

    auto& emitter = effect_->emitters[selected_emitter_];

    // Default grid positions for modules without saved positions
    float default_x[] = {50.0f, 400.0f, 750.0f};
    float y_spacing = 140.0f;

    auto apply_stack = [&](int stack_idx, std::vector<std::unique_ptr<VfxModule>>& stack) {
        float y = 30.0f;
        for (int i = 0; i < static_cast<int>(stack.size()); ++i) {
            auto nid = ne::NodeId(module_node_id(stack_idx, i));
            if (stack[i]->position_set) {
                ne::SetNodePosition(nid, ImVec2(stack[i]->editor_x, stack[i]->editor_y));
                stack[i]->position_set = false;
            } else {
                ne::SetNodePosition(nid, ImVec2(default_x[stack_idx], y));
            }
            y += y_spacing;
        }
    };

    apply_stack(0, emitter.spawn_modules);
    apply_stack(1, emitter.update_modules);
    apply_stack(2, emitter.render_modules);
}

void VfxEditorContext::sync_node_positions() {
    if (!effect_ || selected_emitter_ < 0 || !node_editor_ctx_) return;
    auto& emitter = effect_->emitters[selected_emitter_];

    auto sync_stack = [&](int stack_idx, std::vector<std::unique_ptr<VfxModule>>& stack) {
        for (int i = 0; i < static_cast<int>(stack.size()); ++i) {
            auto pos = ne::GetNodePosition(ne::NodeId(module_node_id(stack_idx, i)));
            stack[i]->editor_x = pos.x;
            stack[i]->editor_y = pos.y;
            stack[i]->position_set = true;
        }
    };
    sync_stack(0, emitter.spawn_modules);
    sync_stack(1, emitter.update_modules);
    sync_stack(2, emitter.render_modules);

    for (auto& node : emitter.expression_graph.nodes) {
        auto pos = ne::GetNodePosition(ne::NodeId(node->node_id));
        node->editor_x = pos.x;
        node->editor_y = pos.y;
        node->position_set = true;
    }
}

// ── Node Editor ─────────────────────────────────────────────────────────

static constexpr float kPinIconSize = 10.0f;
static constexpr float kNodeMinWidth = 160.0f;


static void draw_pin_icon(PinDataType type, bool connected, float size = kPinIconSize) {
    // Text renders at CursorPos.y + CurrLineTextBaseOffset (see imgui TextEx).
    // We match that: circle center = text visual center.
    float text_h = ImGui::GetTextLineHeight();
    float baseline = ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float cx = pos.x + size * 0.5f;
    float cy = pos.y + baseline + text_h * 0.5f;
    float r = size * 0.4f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImColor col = pin_color(type);
    if (connected) {
        dl->AddCircleFilled(ImVec2(cx, cy), r, col);
        dl->AddCircle(ImVec2(cx, cy), r, ImColor(col.Value.x * 0.6f, col.Value.y * 0.6f,
                      col.Value.z * 0.6f, 1.0f), 12, 1.5f);
    } else {
        dl->AddCircleFilled(ImVec2(cx, cy), r, ImColor(col.Value.x * 0.15f, col.Value.y * 0.15f,
                            col.Value.z * 0.15f, 0.8f));
        dl->AddCircle(ImVec2(cx, cy), r, col, 12, 1.5f);
    }
    ImGui::Dummy(ImVec2(size, text_h));
}

void VfxEditorContext::draw_node_editor() {
    if (auto panel = Panel(ui::icon::vfx, ctx_title("Graph").c_str())) {
        if (!effect_ || !node_editor_ctx_) {
            ui::empty_state(ui::icon::vfx, "No effect", "The effect couldn't be read; the console says why");
            return;
        }

        ne::SetCurrentEditor(node_editor_ctx_);

        // The emitters as tabs, the one the graph shows in front (sheet 25).
        {
            std::vector<std::string> names;
            for (const auto& em : effect_->emitters) names.push_back(em.name);
            const auto tabs = ui::tab_strip("##emitters", {.names = names,
                                                           .selected = std::max(selected_emitter_, 0),
                                                           .category = theme::Category::Vfx,
                                                           .add_label = "Add emitter",
                                                           .remove_label = "Remove emitter"});
            if (tabs.selected) {
                sync_node_positions();
                selected_emitter_ = *tabs.selected;
                selected_module_stack_ = -1;
                selected_module_index_ = -1;
                needs_initial_layout_ = true;
            }
            if (tabs.add) {
                sync_node_positions();
                effect_->emitters.push_back(VfxEmitter::make_default());
                auto& em = effect_->emitters.back();
                em.name = "emitter_" + std::to_string(effect_->emitters.size());
                selected_emitter_ = static_cast<int>(effect_->emitters.size()) - 1;
                needs_initial_layout_ = true;
                setup_preview();
            }
            if (tabs.removed) {
                sync_node_positions();
                effect_->emitters.erase(effect_->emitters.begin() + *tabs.removed);
                selected_emitter_ = std::min(selected_emitter_, static_cast<int>(effect_->emitters.size()) - 1);
                selected_module_stack_ = -1;
                selected_module_index_ = -1;
                needs_initial_layout_ = true;
                setup_preview();
            }
            if (tabs.rename) {
                renaming_emitter_ = *tabs.rename;
                rename_text_ = effect_->emitters[static_cast<size_t>(*tabs.rename)].name;
                ui::open_dialog("##rename_emitter");
            }
            if (ui::prompt_dialog("##rename_emitter", {.title = "Rename emitter", .hint = "Its name", .confirm = "Rename"},
                                  rename_text_) == ui::DialogAnswer::Confirm &&
                renaming_emitter_ >= 0 && renaming_emitter_ < static_cast<int>(effect_->emitters.size())) {
                effect_->emitters[static_cast<size_t>(renaming_emitter_)].name = rename_text_;
            }

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Preview speed");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            (void)ui::slider("##speed", time_scale_, 0.0f, 3.0f, ui::Unit::Times, "%.1f");
        }

        if (selected_emitter_ < 0 || selected_emitter_ >= static_cast<int>(effect_->emitters.size())) {
            ui::empty_state(ui::icon::vfx, "No emitter selected", "Pick one above or add one");
            return;
        }

        auto& emitter = effect_->emitters[selected_emitter_];

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

        // Check if a pin has any link connected to it
        auto& mod_graph = emitter.expression_graph;
        auto is_mod_pin_linked = [&](int pid) {
            uint64_t id = static_cast<uint64_t>(pid);
            for (auto& link : mod_graph.links)
                if (link.source_pin_id == id || link.target_pin_id == id)
                    return true;
            return false;
        };

        // Draw module nodes for each stack
        auto draw_stack = [&](int stack_idx, auto& modules) {
            for (int i = 0; i < static_cast<int>(modules.size()); ++i) {
                auto* mod = modules[i].get();
                int nid = module_node_id(stack_idx, i);

                bool is_selected = (stack_idx == selected_module_stack_ &&
                                    i == selected_module_index_);

                ImU32 col = stack_color(stack_idx);
                ne::PushStyleColor(ne::StyleColor_NodeBorder,
                    is_selected ? ImColor(212, 160, 84, 255) : ImColor(52, 54, 58, 255));
                ne::PushStyleVar(ne::StyleVar_NodePadding, ImVec4(10, 8, 10, 8));

                ne::BeginNode(ne::NodeId(nid));
                ImGui::PushID(nid);
                ImGui::BeginVertical("node");

                // ── Header ──
                ImGui::BeginHorizontal("header", ImVec2(kNodeMinWidth, 0));
                    ImGui::Spring(1);
                    ImFont* bold = ImGui::GetIO().Fonts->Fonts.Size > 1
                        ? ImGui::GetIO().Fonts->Fonts[1] : nullptr;
                    if (bold) ImGui::PushFont(bold);
                    ImGui::TextUnformatted(mod->name());
                    if (bold) ImGui::PopFont();
                    if (!mod->enabled) {
                        ImGui::Spring(0, 4.0f);
                        ImGui::TextColored(ImVec4(0.45f, 0.46f, 0.48f, 1.0f), "(off)");
                    }
                    ImGui::Spring(1);
                ImGui::EndHorizontal();
                ImVec2 header_max = ImGui::GetItemRectMax();

                ImGui::Spring(0, 10.0f);

                // ── Pins area ──
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 6.0f));
                ImGui::BeginHorizontal("content", ImVec2(0, 0), 0.0f);
                    ImGui::Spring(0, 0);

                    // Inputs column (left-aligned)
                    auto pins = mod->pin_descriptors();
                    ImGui::BeginVertical("inputs", ImVec2(0, 0), 0.0f);
                        for (int p = 0; p < static_cast<int>(pins.size()); ++p) {
                            if (pins[p].data_type == PinDataType::enum_type) continue;

                            ne::BeginPin(ne::PinId(input_pin_id(nid, p)), ne::PinKind::Input);
                                ne::PinPivotAlignment(ImVec2(0.0f, 0.5f));
                                ne::PinPivotSize(ImVec2(0, 0));
                                draw_pin_icon(pins[p].data_type, is_mod_pin_linked(input_pin_id(nid, p)));
                            ne::EndPin();
                            ImGui::SameLine(0, 4.0f);

                            ImGui::TextUnformatted(display_name(pins[p].name).c_str());
                            ImGui::SameLine(0, 6.0f);

                            draw_value_preview(pins[p]);
                        }
                    ImGui::EndVertical();

                    ImGui::Spring(1);

                    // Output column (right-aligned)
                    ImGui::BeginVertical("outputs", ImVec2(0, 0), 1.0f);
                        // Use same structure as input: icon first for consistent row height
                        ne::BeginPin(ne::PinId(output_pin_id(nid)), ne::PinKind::Output);
                            ne::PinPivotAlignment(ImVec2(1.0f, 0.5f));
                            ne::PinPivotSize(ImVec2(0, 0));
                            draw_pin_icon(PinDataType::float_type, is_mod_pin_linked(output_pin_id(nid)));
                        ne::EndPin();
                        ImGui::SameLine(0, 4.0f);
                        ImGui::TextUnformatted("out");
                    ImGui::EndVertical();

                ImGui::EndHorizontal();
                ImGui::PopStyleVar();

                // Texture preview for InitialTextureModule
                if (mod->type() == ModuleType::initial_texture) {
                    auto* tex_mod = static_cast<InitialTextureModule*>(mod);
                    if (!tex_mod->texture_path.empty() && deps_.icon_cache) {
                        auto resolved = base_path::resolve_asset(
                            tex_mod->texture_path, deps_.project_mgr->active());
                        auto desc = deps_.icon_cache->thumbnail(resolved);
                        if (desc) {
                            ImGui::Spring(0, 4.0f);
                            constexpr float preview_size = 48.0f;
                            ImGui::Image(desc, ImVec2(preview_size, preview_size));
                        }
                    }
                }

                ImGui::EndVertical();
                ImGui::PopID();
                ne::EndNode();

                // Draw header background behind the title (full node width)
                if (ImGui::IsItemVisible()) {
                    auto* bg = ne::GetNodeBackgroundDrawList(ne::NodeId(nid));
                    float rounding = ne::GetStyle().NodeRounding;
                    float bw = ne::GetStyle().NodeBorderWidth * 0.5f;
                    ImVec2 node_pos = ne::GetNodePosition(ne::NodeId(nid));
                    ImVec2 node_size = ne::GetNodeSize(ne::NodeId(nid));
                    ImVec2 hmin(node_pos.x + bw, node_pos.y + bw);
                    ImVec2 hmax(node_pos.x + node_size.x - bw, header_max.y + 4.0f);
                    bg->AddRectFilled(hmin, hmax,
                        IM_COL32(ImU8(col >> 0), ImU8(col >> 8), ImU8(col >> 16), 40),
                        rounding, ImDrawFlags_RoundCornersTop);
                    bg->AddLine(
                        ImVec2(hmin.x, hmax.y),
                        ImVec2(hmax.x, hmax.y),
                        IM_COL32(ImU8(col >> 0), ImU8(col >> 8), ImU8(col >> 16), 80), 1.0f);
                }

                ne::PopStyleVar(1);
                ne::PopStyleColor(1);
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
            auto cpos = ne::ScreenToCanvas(ImGui::GetMousePos());
            context_menu_canvas_x_ = cpos.x;
            context_menu_canvas_y_ = cpos.y;
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
}

// ── Context Menu (replaces Add Module panel) ────────────────────────────

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
        if (kind.stack != stack || !ui::menu_item({.label = kind.label})) continue;
        auto& modules = stack_of(emitter, stack);
        modules.push_back(kind.make());
        needs_initial_layout_ = true;
        selected_module_stack_ = stack;
        selected_module_index_ = static_cast<int>(modules.size()) - 1;
    }
}

void VfxEditorContext::draw_context_menu() {
    if (!effect_ || selected_emitter_ < 0) return;
    auto& emitter = effect_->emitters[selected_emitter_];

    if (ImGui::BeginPopup("##add_module")) {
        for (int stack = 0; stack < 3; ++stack) {
            if (ui::begin_menu(ui::icon::vfx, STACK_NAMES[stack])) {
                draw_add_module_items(emitter, stack);
                ImGui::EndMenu();
            }
        }
        ImGui::Separator();
        if (ui::begin_menu(ui::icon::curve, "Expression")) {
            auto& graph = emitter.expression_graph;
            auto add_expr = [&](const char* label, ExprNodeType type) {
                if (ui::menu_item({.label = label})) {
                    auto* node = graph.add_node(type);
                    node->editor_x = context_menu_canvas_x_;
                    node->editor_y = context_menu_canvas_y_;
                    node->position_set = true;
                }
            };
            ui::menu_heading("Constants");
            add_expr("Float", ExprNodeType::constant_float);
            add_expr("Vec3", ExprNodeType::constant_vec3);
            add_expr("Color", ExprNodeType::constant_vec4);
            ui::menu_heading("Sources");
            add_expr("Time", ExprNodeType::time);
            add_expr("Delta time", ExprNodeType::delta_time);
            add_expr("Random", ExprNodeType::random);
            add_expr("Global wind", ExprNodeType::global_wind);
            ui::menu_heading("Math");
            add_expr("Add", ExprNodeType::add);
            add_expr("Subtract", ExprNodeType::subtract);
            add_expr("Multiply", ExprNodeType::multiply);
            add_expr("Divide", ExprNodeType::divide);
            add_expr("Negate", ExprNodeType::negate);
            add_expr("Abs", ExprNodeType::abs_node);
            add_expr("Sin", ExprNodeType::sin_node);
            add_expr("Cos", ExprNodeType::cos_node);
            ui::menu_heading("Interpolation");
            add_expr("Lerp", ExprNodeType::lerp);
            add_expr("Remap", ExprNodeType::remap);
            add_expr("Clamp", ExprNodeType::clamp);
            ui::menu_heading("Conversion");
            add_expr("Split vec3", ExprNodeType::split_vec3);
            add_expr("Combine vec3", ExprNodeType::combine_vec3);
            add_expr("Split vec4", ExprNodeType::split_vec4);
            add_expr("Combine vec4", ExprNodeType::combine_vec4);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
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
    auto panel = Panel(ui::icon::vfx, ctx_title("Modules").c_str());
    if (!panel) return;
    if (!effect_ || selected_emitter_ < 0 || selected_emitter_ >= static_cast<int>(effect_->emitters.size())) {
        ui::empty_state(ui::icon::vfx, "No emitter selected", "Select an emitter in the graph");
        return;
    }
    auto& emitter = effect_->emitters[static_cast<size_t>(selected_emitter_)];

    ui::section("Emitter", ui::icon::vfx);
    if (auto t = ui::PropertyTable("##emitter")) {
        ui::row("Name", [&] { (void)ui::text_field("##name", emitter.name); });
        ui::row("Max particles", [&] {
            int max_particles = static_cast<int>(emitter.max_particles);
            if (ui::drag_int("##max", max_particles, 10.0f, 100, 1000000).changed) {
                emitter.max_particles = static_cast<uint32_t>(std::max(100, max_particles));
            }
        });
        static constexpr ui::Choice<ParticleBlendMode> BLENDS[] = {
            {ParticleBlendMode::additive, "Additive"}, {ParticleBlendMode::alpha, "Alpha"},
            {ParticleBlendMode::opaque, "Opaque"}};
        ui::row("Blend", [&] { (void)ui::choice("##blend", emitter.blend_mode, BLENDS); });
        ui::row("Start delay", [&] {
            (void)ui::drag("##delay", emitter.start_delay,
                           {.speed = 0.05f, .min = 0.0f, .max = 60.0f, .unit = ui::Unit::Seconds});
        });
        char lifetime[80];
        std::snprintf(lifetime, sizeof(lifetime), "How long it emits. 0 runs for a particle's lifetime, %.1f s.",
                      emitter.lifetime_max());
        ui::row("Duration", lifetime, [&] {
            (void)ui::drag("##duration", emitter.duration,
                           {.speed = 0.05f, .min = 0.0f, .max = 60.0f, .unit = ui::Unit::Seconds});
        });
        ui::row("Loops", "How many times it runs. 0 loops for ever.", [&] {
            int loops = static_cast<int>(emitter.loop_count);
            if (ui::drag_int("##loops", loops, 0.2f, 0, 1000).changed) {
                emitter.loop_count = static_cast<uint32_t>(std::max(0, loops));
            }
        });
        ui::row("Own time", "Off, the graph's Time node reads the engine's clock, so a sin- or cos-driven value "
                            "lands on another phase each time the emitter fires. On, each firing starts its own "
                            "Time at 0, so the graph replays the same way every time.",
                [&] { (void)ui::checkbox("##instanced_time", emitter.instanced_time); });
        char now[96];
        std::snprintf(now, sizeof(now), "%.2f s, loop %u, %s", emitter.emitter_elapsed, emitter.loops_completed,
                      emitter.finished ? "finished" : emitter.paused ? "paused" : "playing");
        ui::row("Now", [&] { ui::readout(now); });
    }

    InspectorContext ictx{};
    ictx.resource_manager = deps_.resource_manager;
    ictx.project_mgr = deps_.project_mgr;
    ictx.content_browser = deps_.content_browser;
    ictx.icons = deps_.icon_cache;

    for (int stack = 0; stack < 3; ++stack) {
        auto& modules = stack_of(emitter, stack);
        ui::section(STACK_NAMES[stack], ui::icon::vfx);
        std::optional<std::size_t> remove;
        for (std::size_t i = 0; i < modules.size(); ++i) {
            VfxModule& module = *modules[i];
            ImGui::PushID(stack);
            ImGui::PushID(static_cast<int>(i));
            bool removed = false;
            if (ui::component_block(module.name(), theme::Category::Vfx, removed, module.enabled)) {
                ui::FadedBody faded(!module.enabled);
                if (draw_vfx_module_fields(module, ictx).committed) resolve_module_assets(module);
            }
            if (removed) remove = i;
            ImGui::PopID();
            ImGui::PopID();
        }
        if (remove) {
            modules.erase(modules.begin() + static_cast<std::ptrdiff_t>(*remove));
            emitter.expression_graph.module_removed(stack, static_cast<int>(*remove));
            needs_initial_layout_ = true;
            selected_module_stack_ = -1;
            selected_module_index_ = -1;
        }
        // Each stack adds its own kind of module.
        ImGui::PushID(stack);
        const std::string add = std::string("Add ") + (stack == 0 ? "spawn" : stack == 1 ? "update" : "render") +
                                " module";
        if (ui::action(ui::icon::add, add.c_str(), ui::ButtonKind::Ghost)) ImGui::OpenPopup("##add_to_stack");
        if (ImGui::BeginPopup("##add_to_stack")) {
            draw_add_module_items(emitter, stack);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

// ── Expression Nodes ────────────────────────────────────────────────────

void VfxEditorContext::draw_expression_nodes() {
    if (!effect_ || selected_emitter_ < 0) return;
    auto& graph = effect_->emitters[selected_emitter_].expression_graph;

    auto is_pin_linked = [&](uint64_t pid) {
        for (auto& link : graph.links)
            if (link.source_pin_id == pid || link.target_pin_id == pid)
                return true;
        return false;
    };

    for (auto& node : graph.nodes) {
        auto nid = ne::NodeId(node->node_id);
        int nid_int = static_cast<int>(node->node_id);
        auto pins = expr_node_pins(node->type);

        // Apply saved position on first frame
        if (node->position_set) {
            ne::SetNodePosition(nid, ImVec2(node->editor_x, node->editor_y));
            node->position_set = false; // only set once, then editor owns it
        }

        ne::PushStyleVar(ne::StyleVar_NodePadding, ImVec4(10, 8, 10, 8));

        ne::BeginNode(nid);
        ImGui::PushID(nid_int);
        ImGui::BeginVertical("node");

        // ── Header ──
        ImGui::BeginHorizontal("header");
            ImGui::Spring(1);
            ImFont* bold = ImGui::GetIO().Fonts->Fonts.Size > 1
                ? ImGui::GetIO().Fonts->Fonts[1] : nullptr;
            if (bold) ImGui::PushFont(bold);
            ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.80f, 1.0f),
                               "%s", expr_node_name(node->type));
            if (bold) ImGui::PopFont();
            ImGui::Spring(1);
        ImGui::EndHorizontal();
        // (expr_header_min not needed — header bg uses node position)
        ImVec2 expr_header_max = ImGui::GetItemRectMax();

        ImGui::Spring(0, 10.0f);

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

        // ── Pins area ──
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 6.0f));
        ImGui::BeginHorizontal("content", ImVec2(0, 0), 0.0f);
            ImGui::Spring(0, 0);

            // Inputs column
            ImGui::BeginVertical("inputs", ImVec2(0, 0), 0.0f);
                if (in_idx.empty()) {
                    ImGui::Dummy(ImVec2(0, 0));
                }
                for (int idx : in_idx) {
                    uint64_t pid = node->node_id * 100 + idx;
                    ne::BeginPin(ne::PinId(static_cast<int>(pid)), ne::PinKind::Input);
                        ne::PinPivotAlignment(ImVec2(0.0f, 0.5f));
                        ne::PinPivotSize(ImVec2(0, 0));
                        draw_pin_icon(pins[idx].type, is_pin_linked(pid));
                    ne::EndPin();
                    ImGui::SameLine(0, 4.0f);
                    ImGui::TextUnformatted(pins[idx].name);
                }
            ImGui::EndVertical();

            ImGui::Spring(1);

            // Outputs column
            ImGui::BeginVertical("outputs", ImVec2(0, 0), 1.0f);
                if (out_idx.empty()) {
                    ImGui::Dummy(ImVec2(0, 0));
                }
                for (int idx : out_idx) {
                    uint64_t pid = node->node_id * 100 + 50 + idx;
                    ne::BeginPin(ne::PinId(static_cast<int>(pid)), ne::PinKind::Output);
                        ne::PinPivotAlignment(ImVec2(1.0f, 0.5f));
                        ne::PinPivotSize(ImVec2(0, 0));
                        draw_pin_icon(pins[idx].type, is_pin_linked(pid));
                    ne::EndPin();
                    ImGui::SameLine(0, 4.0f);
                    ImGui::TextUnformatted(pins[idx].name);
                }
            ImGui::EndVertical();

        ImGui::EndHorizontal();
        ImGui::PopStyleVar();

        ImGui::EndVertical();
        ImGui::PopID();
        ne::EndNode();

        // Draw header background (full node width)
        if (ImGui::IsItemVisible()) {
            auto* bg = ne::GetNodeBackgroundDrawList(nid);
            float rounding = ne::GetStyle().NodeRounding;
            float bw = ne::GetStyle().NodeBorderWidth * 0.5f;
            ImVec2 node_pos = ne::GetNodePosition(nid);
            ImVec2 node_size = ne::GetNodeSize(nid);
            ImVec2 hmin(node_pos.x + bw, node_pos.y + bw);
            ImVec2 hmax(node_pos.x + node_size.x - bw, expr_header_max.y + 4.0f);
            bg->AddRectFilled(hmin, hmax,
                IM_COL32(70, 80, 100, 35),
                rounding, ImDrawFlags_RoundCornersTop);
            bg->AddLine(
                ImVec2(hmin.x, hmax.y),
                ImVec2(hmax.x, hmax.y),
                IM_COL32(70, 80, 100, 80), 1.0f);
        }

        ne::PopStyleVar(1);
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

                // Ensure output → input direction (output pins have index >= 50)
                bool src_is_output = (src % 100) >= 50;
                bool dst_is_output = (dst % 100) >= 50;

                // Reject input→input or output→output
                if (src_is_output == dst_is_output) {
                    ne::RejectNewItem(ImColor(200, 50, 50, 255), 2.0f);
                } else {
                    // Normalize: source should be the output pin
                    if (!src_is_output) std::swap(src, dst);

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

    // Alt+click to disconnect: on a link or on a pin
    if (ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ne::LinkId hovered_link = ne::GetHoveredLink();
        if (hovered_link) {
            graph.remove_link(static_cast<uint64_t>(hovered_link.Get()));
        } else {
            ne::PinId hovered_pin = ne::GetHoveredPin();
            if (hovered_pin) {
                uint64_t pid = static_cast<uint64_t>(hovered_pin.Get());
                // Remove all links connected to this pin
                for (int i = static_cast<int>(graph.links.size()) - 1; i >= 0; --i) {
                    if (graph.links[i].source_pin_id == pid ||
                        graph.links[i].target_pin_id == pid) {
                        graph.remove_link(graph.links[i].link_id);
                    }
                }
            }
        }
    }
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
