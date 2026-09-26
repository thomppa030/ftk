#include "ui/input_mapping_editor_context.hpp"
#include "core/base_path.hpp"
#include "core/input.hpp"
#include "core/input_action_map.hpp"
#include "core/input_context_io.hpp"
#include "core/input_source.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "ui/grouped_picker.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/inset_group.hpp"
#include "ui/kit/key_cap.hpp"
#include "ui/kit/menu.hpp"
#include "ui/kit/panel.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/search.hpp"
#include "ui/kit/strip.hpp"
#include "ui/kit/text_field.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>

namespace fjell {

namespace {

const char* modifier_name(ModifierType type) {
    switch (type) {
        case ModifierType::scale: return "Scale";
        case ModifierType::deadzone: return "Deadzone";
        case ModifierType::negate: return "Negate";
        case ModifierType::swizzle: return "Swizzle";
    }
    return "Scale";
}

// The device a source belongs to.
enum class Device : std::uint8_t { none, keyboard, mouse, pad };

Device device_of(InputSource s) {
    if (s == InputSource::none) return Device::none;
    if (is_keyboard_source(s)) return Device::keyboard;
    if (is_mouse_button_source(s) || is_mouse_axis_source(s)) return Device::mouse;
    if (is_pad_button_source(s) || is_pad_axis_source(s)) return Device::pad;
    return Device::none;
}

const char* device_icon(Device d) {
    switch (d) {
        case Device::keyboard: return ui::icon::keyboard;
        case Device::mouse: return ui::icon::mouse;
        case Device::pad: return ui::icon::gamepad;
        case Device::none: return nullptr;
    }
    return nullptr;
}

const char* device_name(Device d) {
    switch (d) {
        case Device::keyboard: return "Keyboard";
        case Device::mouse: return "Mouse";
        case Device::pad: return "Gamepad";
        case Device::none: return "Other";
    }
    return "Other";
}

// What an axis source means, said once here rather than in every game's
// script. An axis has no press, so the picker is the only place that can
// explain what the action will receive.
const char* source_hint(InputSource s) {
    switch (s) {
        case InputSource::mouse_move_x:
        case InputSource::mouse_move_y:
            return "Pixels moved, up is +Y: do not scale by delta time";
        case InputSource::mouse_scroll_x:
        case InputSource::mouse_scroll_y:
            return "Wheel notches this frame";
        case InputSource::pad_lstick_x:
        case InputSource::pad_lstick_y:
        case InputSource::pad_rstick_x:
        case InputSource::pad_rstick_y:
            return "Stick deflection -1..1: a rate, scale by delta time";
        case InputSource::pad_lt:
        case InputSource::pad_rt:
            return "Trigger pull 0..1";
        default:
            return "";
    }
}

// Sources an axis2d binding pairs automatically, so offering the Y half would
// bind a stick's vertical to an action's horizontal. See
// InputActionMap::evaluate_binding.
bool is_paired_y_source(InputSource s) {
    return s == InputSource::mouse_move_y || s == InputSource::mouse_scroll_y
           || s == InputSource::pad_lstick_y || s == InputSource::pad_rstick_y;
}

std::vector<grouped_picker::Group> build_source_groups() {
    static constexpr Device ORDER[] = {Device::keyboard, Device::mouse, Device::pad};
    std::vector<grouped_picker::Group> groups;
    for (Device device : ORDER) {
        grouped_picker::Group group;
        group.label = device_name(device);
        // Keyboard is long enough that an open header buries the other two.
        group.open = device != Device::keyboard;
        for (InputSource source : input_source_all()) {
            if (device_of(source) != device || is_paired_y_source(source)) continue;
            const std::string name = input_source_to_string(source);
            group.items.push_back({
                .value = name,
                .label = input_source_display_name(source),
                .sublabel = source_hint(source),
                // The JSON name is what a .fjinput holds, so someone who has
                // read the file can search the spelling they saw there.
                .search_text = name,
            });
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

} // namespace

// ── Lifecycle ───────────────────────────────────────────────────────

InputMappingEditorContext::InputMappingEditorContext(InputMappingEditorDeps deps)
    : deps_{deps} {}

EditorContext::DockPreset InputMappingEditorContext::dock_preset() const {
    return {.preview = {"Mappings"}};
}

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

    scan_actions();
    document_opened();
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

std::string InputMappingEditorContext::document() const {
    return fjinput_abs_path_.empty() ? std::string{} : input_context_text(context_);
}

Result<> InputMappingEditorContext::write() {
    if (fjinput_abs_path_.empty()) return make_error("no input mapping is open");
    write_input_context(context_, fjinput_abs_path_);
    FJELL_CORE_INFO("Input mapping saved: {}", fjinput_path_);
    return {};
}

Result<> InputMappingEditorContext::restore(const std::string& text) {
    context_ = input_context_from_text(text, fjinput_path_);
    return {};
}

// ── Rebind capture ──────────────────────────────────────────────────

InputSource InputMappingEditorContext::poll_rebind_source() const {
    // raw_* throughout: the editor runs with the game unfocused, and the
    // gated queries deliberately report nothing in that state. Reading them
    // here made the listener look armed while it could never capture at all.
    for (int k = 32; k < 349; ++k) {
        if (k == GLFW_KEY_ESCAPE) continue;
        if (deps_.input->raw_key_pressed(k)) {
            return static_cast<InputSource>(k);
        }
    }
    for (int b = 0; b < 5; ++b) {
        if (deps_.input->raw_mouse_button_pressed(b)) {
            return static_cast<InputSource>(
                static_cast<int>(InputSource::mouse_left) + b);
        }
    }
    for (int b = 0; b < 15; ++b) {
        if (deps_.input->raw_pad_button_pressed(b)) {
            return static_cast<InputSource>(
                static_cast<int>(InputSource::pad_a) + b);
        }
    }
    return InputSource::none;
}

// ── Draw ────────────────────────────────────────────────────────────

void InputMappingEditorContext::draw(float dt) {
    // The listener takes the first press.
    if (rebinding_) {
        const InputSource captured = poll_rebind_source();
        if (captured != InputSource::none && rebind_binding_idx_ >= 0 &&
            rebind_binding_idx_ < static_cast<int>(context_.bindings.size())) {
            set_source(rebind_binding_idx_, rebind_target_, captured);
            rebinding_ = false;
        }
    }

    draw_mappings();
    draw_shared_panels(dt);

    // Removed after drawing, so no row is drawn from a vector that shrank.
    if (remove_binding_idx_ >= 0) {
        context_.bindings.erase(context_.bindings.begin() + remove_binding_idx_);
        remove_binding_idx_ = -1;
    }
    if (!remove_action_name_.empty()) {
        std::erase_if(context_.bindings, [&](const InputBinding& b) { return b.action == remove_action_name_; });
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

void InputMappingEditorContext::set_source(int binding_idx, RebindTarget target, InputSource source) {
    auto& binding = context_.bindings[static_cast<std::size_t>(binding_idx)];
    switch (target) {
        case RebindTarget::source: binding.source = source; break;
        case RebindTarget::up: binding.up = source; break;
        case RebindTarget::down: binding.down = source; break;
        case RebindTarget::left: binding.left = source; break;
        case RebindTarget::right: binding.right = source; break;
    }
}

ActionType InputMappingEditorContext::action_type(const std::string& action) const {
    for (const auto& a : discovered_actions_) {
        if (a.name == action) return a.type;
    }
    return ActionType::button;
}

// The context's own action, in the header: adding an action to map.
void InputMappingEditorContext::draw_header_actions() {
    if (fjinput_path_.empty()) return;
    ImGui::SameLine();
    if (ui::action(ui::icon::add, "Add action")) {
        action_filter_.clear();
        scan_actions();
        ImGui::OpenPopup("##add_action");
    }
    draw_add_action_popup();
}

void InputMappingEditorContext::draw_mappings() {
    auto panel = ui::Panel(ui::icon::input, ctx_title("Mappings").c_str());
    if (!panel) return;
    if (fjinput_path_.empty()) {
        ui::empty_state(ui::icon::input, "No input context", "Open a .fjinput from the content browser");
        return;
    }

    // The context itself: its name, whether it swallows what it matches,
    // and the device in use now, so bindings can be tried on a real pad.
    if (auto strip = ui::RaisedStrip("##context")) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        (void)ui::text_field("##name", context_.name);
        ImGui::SameLine(0.0f, theme::GAP_L);
        (void)ui::checkbox("##consume", context_.consume);
        ImGui::SameLine();
        ImGui::TextUnformatted("Consume input");
        ImGui::SetItemTooltip("Inputs this context matches don't reach the contexts under it");
        if (deps_.input != nullptr) {
            const bool on_pad = deps_.input->active_device() == InputDevice::gamepad;
            const char* device = on_pad ? "Gamepad" : "Keyboard and mouse";
            const char* icon = on_pad ? ui::icon::gamepad : ui::icon::keyboard;
            const float w = ImGui::CalcTextSize(icon).x + ImGui::CalcTextSize(device).x + theme::GAP_M * 2.0f + 8.0f;
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - w, 0.0f));
            ImGui::TextUnformatted(icon);
            ImGui::SameLine(0.0f, theme::GAP_S);
            ImGui::TextUnformatted(device);
            const auto& pad = deps_.input->pad_name();
            ImGui::SetItemTooltip("%s", on_pad ? pad.c_str() : pad.empty() ? "No gamepad connected" : pad.c_str());
            ImGui::SameLine(0.0f, theme::GAP_S);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled({at.x + 4.0f, at.y + ImGui::GetFrameHeight() * 0.5f}, 3.5f,
                                                        ImGui::GetColorU32(theme::success()));
            ImGui::Dummy({8.0f, ImGui::GetFrameHeight()});
        }
    }
    ImGui::Dummy({0.0f, theme::GAP_S});

    // The actions, in the order their first binding appears.
    std::vector<std::string> actions;
    for (const auto& binding : context_.bindings) {
        if (std::find(actions.begin(), actions.end(), binding.action) == actions.end()) actions.push_back(binding.action);
    }
    if (actions.empty()) {
        ui::empty_state(ui::icon::input, "No actions mapped yet", "Add action in the header, or drop a .fjaction here");
    }
    for (const auto& action : actions) {
        draw_action_section(action, action_type(action));
        ImGui::Dummy({0.0f, theme::GAP_XS});
    }

    accept_action_drop();
}

// A .fjaction dragged from the content browser lands as a binding on the
// action it names. The file is the authority on that name, not its stem: an
// action renamed inside the JSON would otherwise map to a name the runtime
// never resolves.
void InputMappingEditorContext::accept_action_drop() {
    if (!ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->InnerRect, ImGui::GetID("##action_drop"))) {
        return;
    }
    if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
        const std::string rel_path(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize - 1));
        if (std::filesystem::path(rel_path).extension() == ".fjaction") {
            const auto def = parse_action_file(base_path::resolve_asset(rel_path, deps_.project_mgr->active()));
            if (def.name.empty()) {
                FJELL_CORE_WARN("Input mapping: {} has no action name", rel_path);
            } else if (std::none_of(context_.bindings.begin(), context_.bindings.end(),
                                    [&](const InputBinding& b) { return b.action == def.name; })) {
                InputBinding binding;
                binding.action = def.name;
                context_.bindings.push_back(std::move(binding));
                // A drop can name an action the picker never listed, because
                // it scans one directory and the browser reaches anywhere.
                if (std::none_of(discovered_actions_.begin(), discovered_actions_.end(),
                                 [&](const InputActionDef& a) { return a.name == def.name; })) {
                    discovered_actions_.push_back(def);
                    std::sort(discovered_actions_.begin(), discovered_actions_.end(),
                              [](const InputActionDef& a, const InputActionDef& b) { return a.name < b.name; });
                }
            }
        }
    }
    ImGui::EndDragDropTarget();
}

void InputMappingEditorContext::draw_action_section(const std::string& action_name, ActionType type) {
    // Folded, the box says which devices it binds and how many bindings.
    int count = 0;
    bool devices[4] = {};
    for (const auto& b : context_.bindings) {
        if (b.action != action_name) continue;
        ++count;
        if (b.is_composite()) {
            for (InputSource s : {b.up, b.down, b.left, b.right}) devices[static_cast<int>(device_of(s))] = true;
        } else {
            devices[static_cast<int>(device_of(b.source))] = true;
        }
    }
    std::string summary;
    for (Device d : {Device::keyboard, Device::mouse, Device::pad}) {
        if (!devices[static_cast<int>(d)]) continue;
        summary += std::string(summary.empty() ? "" : " \xc2\xb7 ") + device_name(d);
    }
    summary += std::string(summary.empty() ? "" : " \xc2\xb7 ") + std::to_string(count) +
               (count == 1 ? " binding" : " bindings");

    bool remove = false;
    const std::string id = "##action_" + action_name;
    if (auto box = ui::InsetGroup(id.c_str(), {.text = action_name.c_str(),
                                               .named = true,
                                               .remove = &remove,
                                               .remove_tooltip = "Remove the action and its bindings",
                                               .folded_summary = summary.c_str()})) {
        int number = 0;
        for (int i = 0; i < static_cast<int>(context_.bindings.size()); ++i) {
            if (context_.bindings[static_cast<std::size_t>(i)].action != action_name) continue;
            ImGui::PushID(i);
            draw_binding_row(i, ++number, type);
            ImGui::PopID();
        }
        if (number == 0) ui::hint("No bindings yet");

        if (ui::action(ui::icon::add, "Add binding", ui::ButtonKind::Ghost)) {
            InputBinding b;
            b.action = action_name;
            context_.bindings.push_back(std::move(b));
        }
        // Only a 2D axis reads four keys as up, down, left and right.
        if (type == ActionType::axis2d) {
            ImGui::SameLine();
            if (ui::action(ui::icon::keyboard, "Add four keys", ui::ButtonKind::Ghost)) {
                InputBinding b;
                b.action = action_name;
                b.up = InputSource::key_w;
                b.down = InputSource::key_s;
                b.left = InputSource::key_a;
                b.right = InputSource::key_d;
                context_.bindings.push_back(std::move(b));
            }
            ImGui::SetItemTooltip("W, A, S and D as one binding; each can be rebound");
        }
    }
    if (remove) remove_action_name_ = action_name;
}

// ── Binding ─────────────────────────────────────────────────────────

void InputMappingEditorContext::draw_binding_row(int binding_idx, int number, ActionType type) {
    auto& binding = context_.bindings[static_cast<std::size_t>(binding_idx)];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;

    // A fine rule over every binding but the first.
    if (number > 1) {
        dl->AddLine({start.x, start.y - 1.0f}, {start.x + width, start.y - 1.0f},
                    ImGui::GetColorU32(theme::surface_base()));
    }
    // The line's height last frame, to know whether the mouse is on it.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID height_id = ImGui::GetID("##height");
    const float last_h = storage->GetFloat(height_id, ImGui::GetFrameHeight() + 8.0f);
    // Still hovered while one of its own icons is held down, or the icon
    // would vanish between the press and the release that clicks it.
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                         ImGui::IsMouseHoveringRect(start, {start.x + width, start.y + last_h});

    ImGui::SetCursorScreenPos({start.x, start.y + 4.0f});
    ImGui::BeginGroup();

    // Its number, then what it listens to.
    char n[8];
    std::snprintf(n, sizeof(n), "%d", number);
    ImGui::PushFont(theme::mono_font(), theme::SMALL_TEXT);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_disabled());
    ImGui::TextUnformatted(n);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine(0.0f, theme::GAP_M);

    float row_y = ImGui::GetCursorScreenPos().y;
    float after_x = 0.0f;
    if (binding.is_composite()) {
        // A S D in a row, then W centred over S, each cap as wide as its key.
        const float gap = 3.0f;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        // Placed one by one: SameLine() would go back to the line before
        // the cluster, not the row these share.
        const float bottom = at.y + h + gap;
        ImGui::SetCursorScreenPos({at.x, bottom});
        draw_source_button("##left", binding.left, binding_idx, RebindTarget::left, true);
        ImGui::SetCursorScreenPos({ImGui::GetItemRectMax().x + gap, bottom});
        draw_source_button("##down", binding.down, binding_idx, RebindTarget::down, true);
        const float down_min = ImGui::GetItemRectMin().x;
        const float down_max = ImGui::GetItemRectMax().x;
        ImGui::SetCursorScreenPos({down_max + gap, bottom});
        draw_source_button("##right", binding.right, binding_idx, RebindTarget::right, true);
        after_x = ImGui::GetItemRectMax().x;
        // W measured before it is placed, to centre it.
        const std::string up = binding.up == InputSource::none ? "Not bound" : input_source_display_name(binding.up);
        ImGui::PushFont(theme::bold_font(), 0.0f);
        const float up_w = std::max(ImGui::CalcTextSize(up.c_str()).x + 18.0f, 34.0f) +
                           std::floor(ImGui::CalcTextSize(ui::icon::dropdown).x + 10.0f);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos({(down_min + down_max - up_w) * 0.5f, at.y});
        draw_source_button("##up", binding.up, binding_idx, RebindTarget::up, true);
        row_y = at.y;
    } else {
        draw_source_button("##source", binding.source, binding_idx, RebindTarget::source, false);
        after_x = ImGui::GetItemRectMax().x;
    }

    // After a divider, what is done to the value, level with the first row.
    ImGui::SetCursorScreenPos({after_x + theme::GAP_M, row_y});
    const ImVec2 divider = ImGui::GetCursorScreenPos();
    dl->AddLine({divider.x, divider.y + 4.0f}, {divider.x, divider.y + ImGui::GetFrameHeight() - 4.0f},
                ImGui::GetColorU32(theme::surface_highest()));
    ImGui::Dummy({1.0f, ImGui::GetFrameHeight()});
    ImGui::SameLine(0.0f, theme::GAP_M);
    draw_modifier_chips(binding_idx, type);
    ImGui::EndGroup();
    const float h = ImGui::GetItemRectMax().y - start.y + 4.0f;
    storage->SetFloat(height_id, h);

    // On the mouse: remove, at the line's end.
    const float icons_x = start.x + width - ImGui::GetFrameHeight();
    const float icons_y = start.y + 4.0f;
    if (hovered) {
        ImGui::SetCursorScreenPos({icons_x, icons_y});
        if (ui::icon_button("##remove", ui::icon::remove, "Remove binding", ui::ButtonKind::GhostDanger)) {
            remove_binding_idx_ = binding_idx;
        }
    }
    ImGui::SetCursorScreenPos({start.x, start.y + h});
    ImGui::Dummy({width, 0.0f});
}

std::string InputMappingEditorContext::picker_id(int binding_idx, RebindTarget target) {
    // grouped_picker tracks which picker is open by this string alone, so
    // every source needs its own.
    char id[48];
    std::snprintf(id, sizeof(id), "src_%d_%d", binding_idx, static_cast<int>(target));
    return id;
}

void InputMappingEditorContext::draw_source_picker(int binding_idx, RebindTarget target) {
    static constexpr grouped_picker::Config PICKER_CONFIG{
        .search_hint = "Search sources",
        .empty_message = "No matching source.",
        .loading_message = "",
        .loading = false,
        .hide_group_headers = false,
        .size = {300.0F, 420.0F},
    };
    // Gated on is_open(): the source list is built before the call.
    const std::string id = picker_id(binding_idx, target);
    if (std::string picked;
        grouped_picker::is_open(id.c_str()) && grouped_picker::draw(id.c_str(), build_source_groups(), PICKER_CONFIG, picked)) {
        if (const InputSource chosen = input_source_from_string(picked);
            chosen != InputSource::none && binding_idx < static_cast<int>(context_.bindings.size())) {
            set_source(binding_idx, target, chosen);
        }
    }
}

bool InputMappingEditorContext::draw_source_button(const char* id, InputSource source, int binding_idx,
                                                   RebindTarget target, bool compact) {
    const bool listening = rebinding_ && rebind_binding_idx_ == binding_idx && rebind_target_ == target;
    const std::string name = source == InputSource::none ? std::string{} : input_source_display_name(source);
    const auto cap = ui::key_cap({.id = id,
                                  .device_icon = compact ? nullptr : device_icon(device_of(source)),
                                  .label = name,
                                  .listening = listening,
                                  .compact = compact});
    const bool clicked = cap.listen;
    if (clicked) {
        rebinding_ = true;
        rebind_binding_idx_ = binding_idx;
        rebind_target_ = target;
    }
    // The chevron lists every source, for one there is no way to press here:
    // an axis, or a pad button with no pad plugged in. Arming the listener
    // too would have it take the keys typed into the list's search.
    if (cap.list) {
        rebinding_ = false;
        grouped_picker::open(picker_id(binding_idx, target).c_str());
    }
    draw_source_picker(binding_idx, target);
    return clicked;
}

void InputMappingEditorContext::draw_modifier_chips(int binding_idx, ActionType type) {
    auto& binding = context_.bindings[static_cast<std::size_t>(binding_idx)];
    std::optional<std::size_t> remove;
    for (std::size_t mi = 0; mi < binding.modifiers.size(); ++mi) {
        auto& mod = binding.modifiers[mi];
        ImGui::PushID(static_cast<int>(mi));
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
        ImGui::TextUnformatted(modifier_name(mod.type));
        ImGui::PopStyleColor();
        // Negate and Swizzle take no value.
        if (mod.type == ModifierType::scale || mod.type == ModifierType::deadzone) {
            ImGui::SameLine(0.0f, theme::GAP_S);
            ImGui::SetNextItemWidth(64.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::surface_inset());
            (void)ui::drag("##value", mod.value,
                           {.speed = 0.01f, .min = 0.0f, .max = 10.0f,
                            .unit = mod.type == ModifierType::scale ? ui::Unit::Times : ui::Unit::None});
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(0.0f, 0.0f);
        if (ui::icon_button("##remove_mod", ui::icon::clear, "Remove modifier", ui::ButtonKind::GhostDanger)) {
            remove = mi;
        }
        ImGui::SameLine(0.0f, theme::GAP_M);
        ImGui::PopID();
    }
    if (remove) binding.modifiers.erase(binding.modifiers.begin() + static_cast<std::ptrdiff_t>(*remove));

    if (ui::action(ui::icon::add, "Modifier", ui::ButtonKind::Ghost)) ImGui::OpenPopup("##add_modifier");
    if (ImGui::BeginPopup("##add_modifier")) {
        if (ui::menu_item({.label = "Scale"})) binding.modifiers.push_back({ModifierType::scale, 1.0f});
        if (ui::menu_item({.label = "Deadzone"})) binding.modifiers.push_back({ModifierType::deadzone, 0.15f});
        if (ui::menu_item({.label = "Negate"})) binding.modifiers.push_back({ModifierType::negate, 0.0f});
        if (ui::menu_item({.label = "Swizzle", .enabled = type == ActionType::axis2d,
                           .disabled_reason = "Swaps X and Y, so only a 2D axis has it"})) {
            binding.modifiers.push_back({ModifierType::swizzle, 0.0f});
        }
        ImGui::EndPopup();
    }
}

// ── Add Action ──────────────────────────────────────────────────────

void InputMappingEditorContext::draw_add_action_popup() {
    if (!ImGui::BeginPopup("##add_action")) return;
    ImGui::SetNextItemWidth(260.0f);
    ui::search_field("##filter", action_filter_);

    std::vector<std::string> existing;
    for (const auto& b : context_.bindings) existing.push_back(b.action);
    bool any = false;
    const struct {
        const char* heading;
        ActionType type;
    } groups[] = {{"Buttons", ActionType::button}, {"One axis", ActionType::axis1d}, {"Two axes", ActionType::axis2d}};
    for (const auto& group : groups) {
        bool heading = false;
        for (const auto& action : discovered_actions_) {
            if (action.type != group.type || !ui::matches(action.name, action_filter_)) continue;
            if (!heading) {
                ui::menu_heading(group.heading);
                heading = true;
            }
            any = true;
            const bool added = std::find(existing.begin(), existing.end(), action.name) != existing.end();
            if (ui::menu_item({.label = action.name.c_str(), .enabled = !added,
                               .disabled_reason = "Already in this context"})) {
                InputBinding b;
                b.action = action.name;
                context_.bindings.push_back(std::move(b));
            }
        }
    }
    if (!any) ui::hint("No action matches");
    ImGui::EndPopup();
}

} // namespace fjell
