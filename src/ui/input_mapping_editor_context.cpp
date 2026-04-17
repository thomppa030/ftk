#include "ui/input_mapping_editor_context.hpp"
#include "core/base_path.hpp"
#include "core/input.hpp"
#include "core/input_action_map.hpp"
#include "core/input_context_io.hpp"
#include "core/input_source.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "ui/dockspace_layout.hpp"

#include <imgui.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <filesystem>

namespace fjell {

// ── Modifier helpers ────────────────────────────────────────────────

static const char* modifier_type_label(ModifierType type) {
    switch (type) {
        case ModifierType::scale: return "Scale";
        case ModifierType::deadzone: return "Deadzone";
        case ModifierType::negate: return "Negate";
        case ModifierType::swizzle: return "Swizzle";
    }
    return "Scale";
}

static const char* action_type_label(ActionType type) {
    switch (type) {
        case ActionType::button: return "Button";
        case ActionType::axis1d: return "Axis1D";
        case ActionType::axis2d: return "Axis2D";
    }
    return "Button";
}

static ImVec4 action_type_color(ActionType type) {
    switch (type) {
        case ActionType::button: return {0.5f, 0.8f, 0.5f, 1.0f};
        case ActionType::axis1d: return {0.5f, 0.7f, 1.0f, 1.0f};
        case ActionType::axis2d: return {1.0f, 0.7f, 0.4f, 1.0f};
    }
    return {1.0f, 1.0f, 1.0f, 1.0f};
}

// ── Lifecycle ───────────────────────────────────────────────────────

InputMappingEditorContext::InputMappingEditorContext(InputMappingEditorDeps deps)
    : deps_{deps} {}

void InputMappingEditorContext::init() {
    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

void InputMappingEditorContext::shutdown() {
    rebinding_ = false;
}

// ── Open ────────────────────────────────────────────────────────────

void InputMappingEditorContext::open_context(const std::string& fjinput_path) {
    fjinput_path_ = fjinput_path;
    fjinput_abs_path_ = base_path::resolve_asset(fjinput_path, deps_.project_mgr->active());

    context_ = parse_input_context(fjinput_abs_path_);
    name_ = std::filesystem::path(fjinput_path).stem().string();
    dirty_ = false;

    scan_actions();

    FJELL_CORE_INFO("Input mapping editor opened: {}", fjinput_path);
}

void InputMappingEditorContext::open_action(const std::string& fjaction_path) {
    scan_actions();

    auto stem = std::filesystem::path(fjaction_path).stem().string();
    focused_action_ = stem;
    name_ = "Input Mapping";

    FJELL_CORE_INFO("Input mapping editor opened for action: {}", stem);
}

void InputMappingEditorContext::scan_actions() {
    std::string actions_dir = "assets/input/actions";
    if (deps_.project_mgr->active()) {
        auto resolved = deps_.project_mgr->active()->resolve(actions_dir);
        discovered_actions_ = scan_action_files(resolved);
    }

    std::sort(discovered_actions_.begin(), discovered_actions_.end(),
              [](const InputActionDef& a, const InputActionDef& b) {
                  return a.name < b.name;
              });
}

// ── Save ────────────────────────────────────────────────────────────

bool InputMappingEditorContext::save() {
    if (fjinput_abs_path_.empty()) return false;

    write_input_context(context_, fjinput_abs_path_);
    dirty_ = false;
    FJELL_CORE_INFO("Input mapping saved: {}", fjinput_path_);
    return true;
}

// ── Dockspace ───────────────────────────────────────────────────────

std::vector<std::string> InputMappingEditorContext::docked_window_names() const {
    return {
        ctx_title("Actions"),
        ctx_title("Bindings"),
        ctx_title("Context Properties"),
        ctx_title("Live Preview"),
        ctx_title("History"),
        ctx_title("Stats"),
        ctx_title("Console"),
    };
}

void InputMappingEditorContext::setup_dockspace(ImGuiID main_area) {
    enum class EPanel { Actions, Bindings, Properties, LivePreview, History, Stats, Console };
    using N = DockNode;

    static const auto layout = N::horizontal_split(0.20f,
        N::leaf({EPanel::Actions}),
        N::horizontal_split(0.72f,
            N::vertical_split(0.80f,
                N::leaf({EPanel::Bindings}),
                N::leaf({EPanel::Stats, EPanel::Console})),
            N::vertical_split(0.45f,
                N::leaf({EPanel::Properties}),
                N::leaf({EPanel::LivePreview, EPanel::History}))));

    build_dockspace(main_area, layout, docked_window_names());
}

// ── Draw ────────────────────────────────────────────────────────────

void InputMappingEditorContext::draw(float dt) {
    // Check rebind completion — poll raw Input directly because draw() runs
    // after poll_events(), so we can see the Pressed state that update() misses.
    if (rebinding_) {
        InputSource captured = InputSource::none;

        // Keyboard
        for (int k = 32; k < 349; ++k) {
            if (k == GLFW_KEY_ESCAPE) continue; // Escape cancels, handled in shortcuts
            if (deps_.input->is_key_pressed(k)) {
                captured = static_cast<InputSource>(k);
                break;
            }
        }
        // Mouse buttons
        if (captured == InputSource::none) {
            for (int b = 0; b < 5; ++b) {
                if (deps_.input->is_mouse_button_pressed(b)) {
                    captured = static_cast<InputSource>(
                        static_cast<int>(InputSource::mouse_left) + b);
                    break;
                }
            }
        }
        // Gamepad buttons
        if (captured == InputSource::none) {
            for (int b = 0; b < 15; ++b) {
                if (deps_.input->pad_button_pressed(b)) {
                    captured = static_cast<InputSource>(
                        static_cast<int>(InputSource::pad_a) + b);
                    break;
                }
            }
        }

        if (captured != InputSource::none && rebind_binding_idx_ >= 0 &&
            rebind_binding_idx_ < static_cast<int>(context_.bindings.size())) {
            auto& binding = context_.bindings[rebind_binding_idx_];
            switch (rebind_target_) {
                case RebindTarget::source: binding.source = captured; break;
                case RebindTarget::up: binding.up = captured; break;
                case RebindTarget::down: binding.down = captured; break;
                case RebindTarget::left: binding.left = captured; break;
                case RebindTarget::right: binding.right = captured; break;
            }
            dirty_ = true;
            rebinding_ = false;
        }
    }

    draw_actions_panel();
    draw_bindings_panel();
    draw_context_properties();
    draw_live_preview();
    draw_shared_panels(dt);
}

bool InputMappingEditorContext::handle_shortcuts() {
    if (rebinding_ && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        rebinding_ = false;
        return true;
    }
    return false;
}

// ── Actions Panel ───────────────────────────────────────────────────

void InputMappingEditorContext::draw_actions_panel() {
    auto title = ctx_title("Actions");
    if (!ImGui::Begin(title.c_str())) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Refresh")) {
        scan_actions();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d actions)", static_cast<int>(discovered_actions_.size()));

    ImGui::Separator();

    for (const auto& action : discovered_actions_) {
        ImGui::PushID(action.name.c_str());

        // Drag source for adding bindings
        ImGui::Selectable(action.name.c_str(), action.name == focused_action_);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("ACTION_NAME", action.name.c_str(),
                                      action.name.size() + 1);
            ImGui::Text("Add binding: %s", action.name.c_str());
            ImGui::EndDragDropSource();
        }

        // Type badge
        ImGui::SameLine();
        ImGui::TextColored(action_type_color(action.type), "[%s]",
                           action_type_label(action.type));

        // Scroll to focused action
        if (action.name == focused_action_ && !focused_action_.empty()) {
            ImGui::SetScrollHereY(0.5f);
            focused_action_.clear();
        }

        ImGui::PopID();
    }

    ImGui::End();
}

// ── Bindings Panel ──────────────────────────────────────────────────

void InputMappingEditorContext::draw_bindings_panel() {
    auto title = ctx_title("Bindings");
    if (!ImGui::Begin(title.c_str())) {
        ImGui::End();
        return;
    }

    if (fjinput_path_.empty()) {
        ImGui::TextDisabled("No .fjinput file loaded");
        ImGui::TextDisabled("Double-click a .fjinput file in the content browser to open it.");
        ImGui::End();
        return;
    }

    // Drag-drop target: drop an action name to create a new binding
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("ACTION_NAME")) {
            std::string action_name(static_cast<const char*>(payload->Data));
            InputBinding new_binding;
            new_binding.action = action_name;
            context_.bindings.push_back(std::move(new_binding));
            dirty_ = true;
        }
        ImGui::EndDragDropTarget();
    }

    // Group bindings by action
    std::vector<std::string> action_order;
    for (const auto& binding : context_.bindings) {
        if (std::find(action_order.begin(), action_order.end(), binding.action) == action_order.end()) {
            action_order.push_back(binding.action);
        }
    }

    int remove_idx = -1;

    for (const auto& action_name : action_order) {
        // Find action type
        ActionType action_type = ActionType::button;
        for (const auto& a : discovered_actions_) {
            if (a.name == action_name) {
                action_type = a.type;
                break;
            }
        }

        // Count bindings for this action
        int count = 0;
        for (const auto& b : context_.bindings) {
            if (b.action == action_name) ++count;
        }

        ImGui::PushID(action_name.c_str());

        // Header with action name + type badge
        char header[256];
        std::snprintf(header, sizeof(header), "%s  ", action_name.c_str());
        bool open = ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen);

        // Type badge on same line as header
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(action_type_color(action_type), "[%s] (%d)",
                           action_type_label(action_type), count);

        if (open) {
            ImGui::Indent(8.0f);

            for (int i = 0; i < static_cast<int>(context_.bindings.size()); ++i) {
                if (context_.bindings[i].action != action_name) continue;

                ImGui::PushID(i);
                draw_binding_row(i);

                // Remove button
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) {
                    remove_idx = i;
                }

                ImGui::PopID();
            }

            // Add binding buttons
            if (ImGui::SmallButton("+ Simple")) {
                InputBinding new_binding;
                new_binding.action = action_name;
                context_.bindings.push_back(std::move(new_binding));
                dirty_ = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+ Composite")) {
                InputBinding new_binding;
                new_binding.action = action_name;
                new_binding.up = InputSource::none;
                new_binding.down = InputSource::none;
                new_binding.left = InputSource::none;
                new_binding.right = InputSource::none;
                // Mark as composite by setting at least one direction to a sentinel
                // is_composite() checks up != none, so we need to set a real key later
                // For now, use key_w as placeholder to mark it composite
                new_binding.up = InputSource::key_w;
                new_binding.down = InputSource::key_s;
                new_binding.left = InputSource::key_a;
                new_binding.right = InputSource::key_d;
                context_.bindings.push_back(std::move(new_binding));
                dirty_ = true;
            }

            ImGui::Unindent(8.0f);
            ImGui::Spacing();
        }

        ImGui::PopID();
    }

    // Process deferred removal
    if (remove_idx >= 0) {
        context_.bindings.erase(context_.bindings.begin() + remove_idx);
        dirty_ = true;
    }

    ImGui::End();
}

void InputMappingEditorContext::draw_binding_row(int binding_idx) {
    auto& binding = context_.bindings[binding_idx];

    if (binding.is_composite()) {
        // Composite: four direction buttons
        draw_source_button("Up", binding.up, binding_idx, RebindTarget::up);
        ImGui::SameLine();
        draw_source_button("Down", binding.down, binding_idx, RebindTarget::down);
        ImGui::SameLine();
        draw_source_button("Left", binding.left, binding_idx, RebindTarget::left);
        ImGui::SameLine();
        draw_source_button("Right", binding.right, binding_idx, RebindTarget::right);
    } else {
        // Single source button
        draw_source_button("Source", binding.source, binding_idx, RebindTarget::source);
    }

    // Modifier chips
    ImGui::SameLine();
    draw_modifier_chips(binding_idx);
}

bool InputMappingEditorContext::draw_source_button(const char* label, InputSource source,
                                                    int binding_idx, RebindTarget target) {
    bool is_this_rebinding = rebinding_ && rebind_binding_idx_ == binding_idx &&
                             rebind_target_ == target;

    char btn_label[128];
    if (is_this_rebinding) {
        std::snprintf(btn_label, sizeof(btn_label), "%s: [...]##%s", label, label);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.8f, 0.6f, 0.2f, 1.0f});
    } else {
        auto display = source != InputSource::none ? input_source_display_name(source) : "---";
        std::snprintf(btn_label, sizeof(btn_label), "%s: %s##%s", label, display.c_str(), label);
    }

    bool clicked = ImGui::SmallButton(btn_label);

    if (is_this_rebinding) {
        ImGui::PopStyleColor();
    }

    if (clicked && !rebinding_) {
        rebinding_ = true;
        rebind_binding_idx_ = binding_idx;
        rebind_target_ = target;
    }

    return clicked;
}

void InputMappingEditorContext::draw_modifier_chips(int binding_idx) {
    auto& binding = context_.bindings[binding_idx];

    for (int mi = 0; mi < static_cast<int>(binding.modifiers.size()); ++mi) {
        auto& mod = binding.modifiers[mi];
        ImGui::PushID(mi + 1000); // offset to avoid ID collision with source buttons

        char chip[64];
        if (mod.type == ModifierType::negate || mod.type == ModifierType::swizzle) {
            std::snprintf(chip, sizeof(chip), "%s", modifier_type_label(mod.type));
        } else {
            std::snprintf(chip, sizeof(chip), "%s: %.2f", modifier_type_label(mod.type), mod.value);
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.3f, 0.3f, 0.5f, 1.0f});
        if (ImGui::SmallButton(chip)) {
            editing_modifier_binding_ = binding_idx;
            editing_modifier_idx_ = mi;
            ImGui::OpenPopup("edit_modifier");
        }
        ImGui::PopStyleColor();

        // Modifier edit popup
        if (editing_modifier_binding_ == binding_idx && editing_modifier_idx_ == mi) {
            if (ImGui::BeginPopup("edit_modifier")) {
                int type_idx = static_cast<int>(mod.type);
                const char* type_names[] = {"Scale", "Deadzone", "Negate", "Swizzle"};
                if (ImGui::Combo("Type", &type_idx, type_names, 4)) {
                    mod.type = static_cast<ModifierType>(type_idx);
                    dirty_ = true;
                }

                if (mod.type == ModifierType::scale || mod.type == ModifierType::deadzone) {
                    if (ImGui::DragFloat("Value", &mod.value, 0.01f, 0.0f, 10.0f)) {
                        dirty_ = true;
                    }
                }

                if (ImGui::Button("Remove")) {
                    binding.modifiers.erase(binding.modifiers.begin() + mi);
                    dirty_ = true;
                    editing_modifier_binding_ = -1;
                    editing_modifier_idx_ = -1;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }
        }

        ImGui::SameLine();
        ImGui::PopID();
    }

    // Add modifier button
    ImGui::PushID(9999);
    if (ImGui::SmallButton("+")) {
        binding.modifiers.push_back({ModifierType::scale, 1.0f});
        dirty_ = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add modifier");
    }
    ImGui::PopID();
}

// ── Context Properties ──────────────────────────────────────────────

void InputMappingEditorContext::draw_context_properties() {
    auto title = ctx_title("Context Properties");
    if (!ImGui::Begin(title.c_str())) {
        ImGui::End();
        return;
    }

    if (fjinput_path_.empty()) {
        ImGui::TextDisabled("No context loaded");
        ImGui::End();
        return;
    }

    // Name
    char name_buf[128];
    std::snprintf(name_buf, sizeof(name_buf), "%s", context_.name.c_str());
    if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
        context_.name = name_buf;
        dirty_ = true;
    }

    // Consume
    if (ImGui::Checkbox("Consume Input", &context_.consume)) {
        dirty_ = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When enabled, matched bindings prevent\nlower contexts from processing the same input");
    }

    ImGui::Separator();

    // File info
    ImGui::TextDisabled("File: %s", fjinput_path_.c_str());
    ImGui::TextDisabled("Bindings: %d", static_cast<int>(context_.bindings.size()));

    if (dirty_) {
        ImGui::TextColored({1.0f, 0.8f, 0.3f, 1.0f}, "(unsaved changes)");
    }

    ImGui::End();
}

// ── Live Preview ────────────────────────────────────────────────────

void InputMappingEditorContext::draw_live_preview() {
    auto title = ctx_title("Live Preview");
    if (!ImGui::Begin(title.c_str())) {
        ImGui::End();
        return;
    }

    // Check if context is active in the runtime stack
    const auto& stack = deps_.input_action_map->context_stack();
    bool context_active = std::find(stack.begin(), stack.end(), context_.name) != stack.end();

    if (!context_active) {
        ImGui::TextDisabled("Context '%s' is not active.", context_.name.c_str());
        ImGui::TextDisabled("Start play mode to see live input.");
        ImGui::End();
        return;
    }

    const auto& states = deps_.input_action_map->states();

    // Show state for each action referenced by this context
    std::vector<std::string> shown_actions;
    for (const auto& binding : context_.bindings) {
        if (std::find(shown_actions.begin(), shown_actions.end(), binding.action) == shown_actions.end()) {
            shown_actions.push_back(binding.action);
        }
    }

    for (const auto& action_name : shown_actions) {
        auto it = states.find(action_name);
        if (it == states.end()) continue;

        const auto& state = it->second;
        ImGui::PushID(action_name.c_str());

        ImGui::Text("%s", action_name.c_str());
        ImGui::SameLine(150.0f);

        if (state.type == ActionType::button) {
            // Button indicators
            auto indicator = [](const char* label, bool active) {
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.2f, 1.0f, 0.3f, 1.0f});
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.4f, 0.4f, 0.4f, 1.0f});
                }
                ImGui::Text("%s", label);
                ImGui::PopStyleColor();
            };

            indicator("P", state.pressed);
            ImGui::SameLine();
            indicator("H", state.held);
            ImGui::SameLine();
            indicator("R", state.released);
        } else if (state.type == ActionType::axis1d) {
            // Horizontal bar
            float normalized = (state.value + 1.0f) * 0.5f; // map -1..1 to 0..1
            ImGui::ProgressBar(normalized, ImVec2{120.0f, 0.0f});
            ImGui::SameLine();
            ImGui::Text("%.2f", state.value);
        } else if (state.type == ActionType::axis2d) {
            // Crosshair widget
            ImVec2 size{60.0f, 60.0f};
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 center{pos.x + size.x * 0.5f, pos.y + size.y * 0.5f};

            auto* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y},
                                     IM_COL32(30, 30, 30, 255));
            draw_list->AddLine({center.x, pos.y}, {center.x, pos.y + size.y},
                               IM_COL32(80, 80, 80, 255));
            draw_list->AddLine({pos.x, center.y}, {pos.x + size.x, center.y},
                               IM_COL32(80, 80, 80, 255));

            // Dot at current value
            float dot_x = center.x + state.axis.x * size.x * 0.5f;
            float dot_y = center.y - state.axis.y * size.y * 0.5f; // Y inverted
            draw_list->AddCircleFilled({dot_x, dot_y}, 4.0f,
                                       IM_COL32(50, 200, 80, 255));

            ImGui::Dummy(size);
            ImGui::SameLine();
            ImGui::Text("(%.2f, %.2f)", state.axis.x, state.axis.y);
        }

        ImGui::PopID();
    }

    ImGui::End();
}

} // namespace fjell
