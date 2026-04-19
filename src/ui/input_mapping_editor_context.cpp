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

static ImU32 action_type_color(ActionType type) {
    switch (type) {
        case ActionType::button: return IM_COL32(120, 200, 120, 255);
        case ActionType::axis1d: return IM_COL32(120, 170, 255, 255);
        case ActionType::axis2d: return IM_COL32(255, 180, 100, 255);
    }
    return IM_COL32(255, 255, 255, 255);
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

// ── Open / Save ─────────────────────────────────────────────────────

void InputMappingEditorContext::open_context(const std::string& fjinput_path) {
    fjinput_path_ = fjinput_path;
    fjinput_abs_path_ = base_path::resolve_asset(fjinput_path, deps_.project_mgr->active());

    context_ = parse_input_context(fjinput_abs_path_);
    name_ = std::filesystem::path(fjinput_path).stem().string();
    dirty_ = false;

    scan_actions();
}

void InputMappingEditorContext::open_action(const std::string& /*fjaction_path*/) {
    scan_actions();
    name_ = "Input Mapping";
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
        ctx_title("Mappings"),
        ctx_title("History"),
        ctx_title("Console"),
    };
}

void InputMappingEditorContext::setup_dockspace(ImGuiID main_area) {
    enum class EPanel { Mappings, History, Console };
    using N = DockNode;

    static const auto layout = N::vertical_split(0.85f,
        N::leaf({EPanel::Mappings}),
        N::leaf({EPanel::History, EPanel::Console}));

    build_dockspace(main_area, layout, docked_window_names());
}

// ── Draw ────────────────────────────────────────────────────────────

void InputMappingEditorContext::draw(float dt) {
    // Poll raw Input for rebind completion
    if (rebinding_) {
        InputSource captured = InputSource::none;

        for (int k = 32; k < 349; ++k) {
            if (k == GLFW_KEY_ESCAPE) continue;
            if (deps_.input->is_key_pressed(k)) {
                captured = static_cast<InputSource>(k);
                break;
            }
        }
        if (captured == InputSource::none) {
            for (int b = 0; b < 5; ++b) {
                if (deps_.input->is_mouse_button_pressed(b)) {
                    captured = static_cast<InputSource>(
                        static_cast<int>(InputSource::mouse_left) + b);
                    break;
                }
            }
        }
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

    draw_mappings();
    draw_shared_panels(dt);

    // Deferred removal
    if (remove_binding_idx_ >= 0) {
        context_.bindings.erase(context_.bindings.begin() + remove_binding_idx_);
        dirty_ = true;
        remove_binding_idx_ = -1;
    }
    if (!remove_action_name_.empty()) {
        auto it = std::remove_if(context_.bindings.begin(), context_.bindings.end(),
            [&](const InputBinding& b) { return b.action == remove_action_name_; });
        if (it != context_.bindings.end()) {
            context_.bindings.erase(it, context_.bindings.end());
            dirty_ = true;
        }
        remove_action_name_.clear();
    }
}

bool InputMappingEditorContext::handle_shortcuts() {
    if (rebinding_ && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        rebinding_ = false;
        return true;
    }
    return false;
}

// ── Mappings (single main panel) ────────────────────────────────────

void InputMappingEditorContext::draw_mappings() {
    auto title = ctx_title("Mappings");

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16.0f, 12.0f});
    if (!ImGui::Begin(title.c_str())) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::PopStyleVar();

    if (fjinput_path_.empty()) {
        ImGui::TextDisabled("No .fjinput file loaded.");
        ImGui::End();
        return;
    }

    // Header bar: context name, consume checkbox, add action
    {
        ImGui::SetNextItemWidth(200.0f);
        char name_buf[128];
        std::snprintf(name_buf, sizeof(name_buf), "%s", context_.name.c_str());
        if (ImGui::InputText("##ContextName", name_buf, sizeof(name_buf))) {
            context_.name = name_buf;
            dirty_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Context name");
        }

        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::Checkbox("Consume", &context_.consume)) {
            dirty_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Consume matched inputs, blocking lower contexts");
        }

        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::Button("+ Add Action")) {
            action_filter_[0] = '\0';
            scan_actions();
            ImGui::OpenPopup("add_action_popup");
        }

        if (dirty_) {
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::TextColored({1.0f, 0.8f, 0.3f, 1.0f}, "(unsaved)");
        }

        draw_add_action_popup();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Spacing();

    // Collect unique actions in binding order
    std::vector<std::string> action_order;
    for (const auto& binding : context_.bindings) {
        if (std::find(action_order.begin(), action_order.end(), binding.action) == action_order.end()) {
            action_order.push_back(binding.action);
        }
    }

    if (action_order.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No actions mapped. Click '+ Add Action' to get started.");
    }

    // Draw each action section
    for (const auto& action_name : action_order) {
        ActionType type = ActionType::button;
        for (const auto& a : discovered_actions_) {
            if (a.name == action_name) {
                type = a.type;
                break;
            }
        }
        draw_action_section(action_name, type);
    }

    ImGui::End();
}

void InputMappingEditorContext::draw_action_section(const std::string& action_name,
                                                     ActionType type) {
    ImGui::PushID(action_name.c_str());

    // Action header with colored left strip
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    float avail_w = ImGui::GetContentRegionAvail().x;
    float header_h = ImGui::GetFrameHeight();

    // Colored type indicator strip
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cursor, {cursor.x + 4.0f, cursor.y + header_h},
                      action_type_color(type), 2.0f);

    ImGui::Indent(14.0f);

    bool open = ImGui::TreeNodeEx("##action", ImGuiTreeNodeFlags_DefaultOpen |
                                               ImGuiTreeNodeFlags_AllowOverlap);

    ImGui::SameLine();
    ImGui::Text("%s", action_name.c_str());
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextDisabled("[%s]", action_type_label(type));

    // Right-aligned remove button
    float remove_w = ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(avail_w - remove_w);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.6f, 0.3f, 0.3f, 1.0f});
    if (ImGui::SmallButton("Remove")) {
        remove_action_name_ = action_name;
    }
    ImGui::PopStyleColor(2);

    if (open) {
        ImGui::Spacing();

        // Binding rows
        for (int i = 0; i < static_cast<int>(context_.bindings.size()); ++i) {
            if (context_.bindings[i].action != action_name) continue;

            ImGui::PushID(i);
            draw_binding_row(i);
            ImGui::PopID();
            ImGui::Spacing();
        }

        // Add binding buttons
        ImGui::Spacing();
        ImGui::Indent(20.0f);
        if (ImGui::SmallButton("+ Binding")) {
            InputBinding b;
            b.action = action_name;
            context_.bindings.push_back(std::move(b));
            dirty_ = true;
        }
        if (type == ActionType::axis2d) {
            ImGui::SameLine(0.0f, 8.0f);
            if (ImGui::SmallButton("+ Composite")) {
                InputBinding b;
                b.action = action_name;
                b.up = InputSource::key_w;
                b.down = InputSource::key_s;
                b.left = InputSource::key_a;
                b.right = InputSource::key_d;
                context_.bindings.push_back(std::move(b));
                dirty_ = true;
            }
        }
        ImGui::Unindent(20.0f);

        ImGui::TreePop();
    }

    ImGui::Unindent(14.0f);
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::PopID();
}

// ── Binding row ─────────────────────────────────────────────────────

void InputMappingEditorContext::draw_binding_row(int binding_idx) {
    auto& binding = context_.bindings[binding_idx];

    ImGui::Indent(20.0f);

    if (binding.is_composite()) {
        draw_source_button("Up", binding.up, binding_idx, RebindTarget::up);
        ImGui::SameLine(0.0f, 6.0f);
        draw_source_button("Dn", binding.down, binding_idx, RebindTarget::down);
        ImGui::SameLine(0.0f, 6.0f);
        draw_source_button("Lt", binding.left, binding_idx, RebindTarget::left);
        ImGui::SameLine(0.0f, 6.0f);
        draw_source_button("Rt", binding.right, binding_idx, RebindTarget::right);
    } else {
        draw_source_button("Key", binding.source, binding_idx, RebindTarget::source);
    }

    // Modifiers
    ImGui::SameLine(0.0f, 12.0f);
    draw_modifier_chips(binding_idx);

    // Remove binding
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.5f, 0.5f, 0.5f, 1.0f});
    if (ImGui::SmallButton("x")) {
        remove_binding_idx_ = binding_idx;
    }
    ImGui::PopStyleColor();

    ImGui::Unindent(20.0f);
}

bool InputMappingEditorContext::draw_source_button(const char* label, InputSource source,
                                                    int binding_idx, RebindTarget target) {
    bool is_this_rebinding = rebinding_ && rebind_binding_idx_ == binding_idx &&
                             rebind_target_ == target;

    char btn_label[128];
    if (is_this_rebinding) {
        std::snprintf(btn_label, sizeof(btn_label), "%s: ...##%s", label, label);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.7f, 0.5f, 0.15f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.8f, 0.6f, 0.2f, 1.0f});
    } else if (source == InputSource::none) {
        std::snprintf(btn_label, sizeof(btn_label), "%s: ---##%s", label, label);
    } else {
        auto display = input_source_display_name(source);
        std::snprintf(btn_label, sizeof(btn_label), "%s: %s##%s", label, display.c_str(), label);
    }

    bool clicked = ImGui::SmallButton(btn_label);

    if (is_this_rebinding) {
        ImGui::PopStyleColor(2);
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
        ImGui::PushID(mi + 1000);

        char chip[64];
        if (mod.type == ModifierType::negate || mod.type == ModifierType::swizzle) {
            std::snprintf(chip, sizeof(chip), "%s", modifier_type_label(mod.type));
        } else {
            std::snprintf(chip, sizeof(chip), "%s:%.2f", modifier_type_label(mod.type), mod.value);
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.25f, 0.25f, 0.4f, 1.0f});
        if (ImGui::SmallButton(chip)) {
            ImGui::OpenPopup("edit_mod");
        }
        ImGui::PopStyleColor();

        if (ImGui::BeginPopup("edit_mod")) {
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
            if (ImGui::Button("Remove Modifier")) {
                binding.modifiers.erase(binding.modifiers.begin() + mi);
                dirty_ = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine(0.0f, 4.0f);
        ImGui::PopID();
    }

    // Add modifier
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.2f, 0.2f, 0.3f, 1.0f});
    if (ImGui::SmallButton("+mod")) {
        binding.modifiers.push_back({ModifierType::scale, 1.0f});
        dirty_ = true;
    }
    ImGui::PopStyleColor();
}

// ── Add Action popup ────────────────────────────────────────────────

void InputMappingEditorContext::draw_add_action_popup() {
    if (!ImGui::BeginPopup("add_action_popup")) return;

    ImGui::Text("Add Action");
    ImGui::Separator();

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##filter", "Filter...", action_filter_, sizeof(action_filter_));

    // Collect actions already in the context
    std::vector<std::string> existing;
    for (const auto& b : context_.bindings) {
        if (std::find(existing.begin(), existing.end(), b.action) == existing.end()) {
            existing.push_back(b.action);
        }
    }

    ImGui::BeginChild("action_list", ImVec2{250.0f, 200.0f}, ImGuiChildFlags_Borders);

    for (const auto& action : discovered_actions_) {
        // Filter
        if (action_filter_[0] != '\0') {
            // Case-insensitive substring match
            std::string name_lower = action.name;
            std::string filter_lower(action_filter_);
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);
            if (name_lower.find(filter_lower) == std::string::npos) continue;
        }

        // Skip already-added actions
        bool already = std::find(existing.begin(), existing.end(), action.name) != existing.end();
        if (already) {
            ImGui::TextDisabled("%s [%s] (added)", action.name.c_str(),
                                action_type_label(action.type));
            continue;
        }

        char label[256];
        std::snprintf(label, sizeof(label), "%s [%s]", action.name.c_str(),
                      action_type_label(action.type));

        if (ImGui::Selectable(label)) {
            // Add an empty binding for this action
            InputBinding b;
            b.action = action.name;
            context_.bindings.push_back(std::move(b));
            dirty_ = true;
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::EndChild();
    ImGui::EndPopup();
}

} // namespace fjell
