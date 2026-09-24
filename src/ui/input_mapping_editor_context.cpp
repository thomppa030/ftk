#include "ui/input_mapping_editor_context.hpp"
#include "core/base_path.hpp"
#include "core/input.hpp"
#include "core/input_action_map.hpp"
#include "core/input_context_io.hpp"
#include "core/input_source.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "ui/dockspace_layout.hpp"
#include "ui/grouped_picker.hpp"
#include "ui/kit/search.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <filesystem>

namespace fjell {

// ── Visual helpers ──────────────────────────────────────────────────

namespace {

ImU32 to_imu32(ImVec4 v) { return ImGui::ColorConvertFloat4ToU32(v); }

// Engine palette → ImU32 (uses sRGB→linear from theme.hpp)
ImU32 col_text_muted()  { return to_imu32(theme::srgb(0.447F, 0.455F, 0.486F)); }

const char* modifier_type_label(ModifierType type) {
    switch (type) {
        case ModifierType::scale: return "Scale";
        case ModifierType::deadzone: return "Deadzone";
        case ModifierType::negate: return "Negate";
        case ModifierType::swizzle: return "Swizzle";
    }
    return "Scale";
}

const char* action_type_label(ActionType type) {
    switch (type) {
        case ActionType::button: return "Button";
        case ActionType::axis1d: return "Axis1D";
        case ActionType::axis2d: return "Axis2D";
    }
    return "Button";
}

const char* action_type_short(ActionType type) {
    switch (type) {
        case ActionType::button: return "BTN";
        case ActionType::axis1d: return "1D";
        case ActionType::axis2d: return "2D";
    }
    return "?";
}

// Action type colors — tuned to the engine palette: amber for Button (matches accent),
// muted blue for Axis1D, sage-tinted teal for Axis2D.
ImU32 action_type_color(ActionType type) {
    switch (type) {
        case ActionType::button: return to_imu32(theme::accent());                       // amber
        case ActionType::axis1d: return to_imu32(theme::srgb(0.486F, 0.624F, 0.808F));   // muted blue
        case ActionType::axis2d: return to_imu32(theme::srgb(0.494F, 0.749F, 0.557F));   // sage (success)
    }
    return col_text_muted();
}

// Source category: keyboard / mouse / pad / none.
enum class SrcKind : std::uint8_t { none, keyboard, mouse, pad };

SrcKind source_kind(InputSource s) {
    if (s == InputSource::none) return SrcKind::none;
    if (is_keyboard_source(s)) return SrcKind::keyboard;
    if (is_mouse_button_source(s) || is_mouse_axis_source(s)) return SrcKind::mouse;
    if (is_pad_button_source(s) || is_pad_axis_source(s)) return SrcKind::pad;
    return SrcKind::none;
}

const char* source_kind_tag(SrcKind k) {
    switch (k) {
        case SrcKind::keyboard: return "KB";
        case SrcKind::mouse:    return "M";
        case SrcKind::pad:      return "PAD";
        case SrcKind::none:     return "";
    }
    return "";
}

ImU32 source_kind_color(SrcKind k) {
    switch (k) {
        case SrcKind::keyboard: return to_imu32(theme::srgb(0.486F, 0.624F, 0.808F));
        case SrcKind::mouse:    return to_imu32(theme::accent());
        case SrcKind::pad:      return to_imu32(theme::srgb(0.494F, 0.749F, 0.557F));
        case SrcKind::none:     return col_text_muted();
    }
    return col_text_muted();
}

const char* source_kind_group(SrcKind k) {
    switch (k) {
        case SrcKind::keyboard: return "Keyboard";
        case SrcKind::mouse:    return "Mouse";
        case SrcKind::pad:      return "Gamepad";
        case SrcKind::none:     return "Other";
    }
    return "Other";
}

// What an axis source means, said once here rather than in every game's
// script. An axis has no press, so the row is the only place a picker can
// explain what the action will receive.
const char* source_hint(InputSource s) {
    switch (s) {
        case InputSource::mouse_move_x:
        case InputSource::mouse_move_y:
            return "Pixels moved, up is +Y — do not scale by delta time";
        case InputSource::mouse_scroll_x:
        case InputSource::mouse_scroll_y:
            return "Wheel notches this frame";
        case InputSource::pad_lstick_x:
        case InputSource::pad_lstick_y:
        case InputSource::pad_rstick_x:
        case InputSource::pad_rstick_y:
            return "Stick deflection -1..1 — a rate, scale by delta time";
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
    using Kind = SrcKind;
    static constexpr Kind ORDER[] = {Kind::keyboard, Kind::mouse, Kind::pad};

    std::vector<grouped_picker::Group> groups;
    for (Kind kind : ORDER) {
        grouped_picker::Group group;
        group.label = source_kind_group(kind);
        // Keyboard is long enough that an open header buries the other two.
        group.open = (kind != Kind::keyboard);

        for (InputSource source : input_source_all()) {
            if (source_kind(source) != kind || is_paired_y_source(source)) {
                continue;
            }
            const std::string name = input_source_to_string(source);
            group.items.push_back({
                .value = name,
                .label = input_source_display_name(source),
                .sublabel = source_hint(source),
                .preview = 0,
                .tag = source_kind_tag(kind),
                .tag_color = source_kind_color(kind),
                // The JSON name is what a .fjinput holds, so someone who has
                // read the file can search the spelling they saw there.
                .search_text = name,
            });
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

// Draw a small colored pill (used for action type and source kind tags).
void draw_pill(const char* text, ImU32 color) {
    ImVec2 text_size = ImGui::CalcTextSize(text);
    ImVec2 pad{6.0F, 2.0F};
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1{p0.x + text_size.x + pad.x * 2.0F, p0.y + text_size.y + pad.y * 2.0F};

    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, color, 4.0F);
    dl->AddText({p0.x + pad.x, p0.y + pad.y}, IM_COL32(20, 22, 26, 255), text);

    ImGui::Dummy({text_size.x + pad.x * 2.0F, text_size.y + pad.y * 2.0F});
}

// Push the bold font (Fonts[1] if loaded by theme::load_font).
bool push_bold_font() {
    auto& io = ImGui::GetIO();
    if (io.Fonts->Fonts.Size > 1) {
        ImGui::PushFont(io.Fonts->Fonts[1]);
        return true;
    }
    return false;
}

} // namespace

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
    // Poll raw Input for rebind completion
    if (rebinding_) {
        InputSource captured = poll_rebind_source();

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
            rebinding_ = false;
        }
    }

    draw_mappings();
    draw_shared_panels(dt);

    // Deferred removal
    if (remove_binding_idx_ >= 0) {
        context_.bindings.erase(context_.bindings.begin() + remove_binding_idx_);
        remove_binding_idx_ = -1;
    }
    if (!remove_action_name_.empty()) {
        auto it = std::remove_if(context_.bindings.begin(), context_.bindings.end(),
            [&](const InputBinding& b) { return b.action == remove_action_name_; });
        if (it != context_.bindings.end()) {
            context_.bindings.erase(it, context_.bindings.end());
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

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{20.0F, 16.0F});
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

    // ── Header row (no separate box, just inline controls) ────────
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Context");
        ImGui::SameLine(0.0F, 8.0F);

        ImGui::SetNextItemWidth(220.0F);
        char name_buf[128];
        std::snprintf(name_buf, sizeof(name_buf), "%s", context_.name.c_str());
        if (ImGui::InputText("##ContextName", name_buf, sizeof(name_buf))) {
            context_.name = name_buf;
        }

        ImGui::SameLine(0.0F, 20.0F);
        (void)ImGui::Checkbox("Consume", &context_.consume);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Consume matched inputs, blocking lower contexts");
        }

        // Which device is live right now, so bindings can be checked against
        // a real controller without writing a script to print it.
        if (deps_.input != nullptr) {
            ImGui::SameLine(0.0F, 20.0F);
            const bool on_pad = deps_.input->active_device() == InputDevice::gamepad;
            ImGui::TextDisabled("Active:");
            ImGui::SameLine(0.0F, 6.0F);
            if (on_pad) {
                ImGui::TextColored(theme::accent(), "Gamepad");
            } else {
                ImGui::TextUnformatted("Keyboard/Mouse");
            }
            if (ImGui::IsItemHovered()) {
                const auto& pad = deps_.input->pad_name();
                ImGui::SetTooltip("%s", pad.empty()
                                            ? "No gamepad connected"
                                            : pad.c_str());
            }
        }

        // Right-aligned: unsaved indicator + Add Action button
        const char* add_label = "+  Add Action";
        float add_w = ImGui::CalcTextSize(add_label).x + ImGui::GetStyle().FramePadding.x * 2.0F;

        const char* unsaved_text = "● unsaved";
        float unsaved_w = has_unsaved_changes() ? ImGui::CalcTextSize(unsaved_text).x + 16.0F : 0.0F;

        float right_edge = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(0.0F, std::max(8.0F, right_edge - add_w - unsaved_w - 4.0F));

        if (has_unsaved_changes()) {
            ImGui::TextColored(theme::accent(), "%s", unsaved_text);
            ImGui::SameLine(0.0F, 16.0F);
        }

        if (ImGui::Button(add_label)) {
            action_filter_.clear();
            scan_actions();
            ImGui::OpenPopup("add_action_popup");
        }

        draw_add_action_popup();
    }

    ImGui::Dummy({0.0F, 8.0F});
    ImGui::Separator();
    ImGui::Dummy({0.0F, 4.0F});

    // ── Collect unique actions in binding order ───────────────────
    std::vector<std::string> action_order;
    for (const auto& binding : context_.bindings) {
        if (std::find(action_order.begin(), action_order.end(), binding.action) == action_order.end()) {
            action_order.push_back(binding.action);
        }
    }

    if (action_order.empty()) {
        ImGui::Dummy({0.0F, 24.0F});
        ImVec2 win_size = ImGui::GetContentRegionAvail();
        const char* msg = "No actions mapped yet.";
        const char* hint = "Drop a .fjaction here, or click \"+ Add Action\" above.";
        float msg_w = ImGui::CalcTextSize(msg).x;
        float hint_w = ImGui::CalcTextSize(hint).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (win_size.x - msg_w) * 0.5F);
        ImGui::TextUnformatted(msg);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (win_size.x - hint_w) * 0.5F);
        ImGui::TextDisabled("%s", hint);
    }

    // ── Draw each action section ──────────────────────────────────
    for (size_t i = 0; i < action_order.size(); ++i) {
        const auto& action_name = action_order[i];
        ActionType type = ActionType::button;
        for (const auto& a : discovered_actions_) {
            if (a.name == action_name) {
                type = a.type;
                break;
            }
        }
        draw_action_section(action_name, type);

        if (i + 1 < action_order.size()) {
            ImGui::Dummy({0.0F, 2.0F});
            ImGui::Separator();
            ImGui::Dummy({0.0F, 6.0F});
        }
    }

    accept_action_drop();

    ImGui::End();
}

// A .fjaction dragged from the content browser lands as a binding on the
// action it names. The file is the authority on that name, not its stem: an
// action renamed inside the JSON would otherwise map to a name the runtime
// never resolves.
void InputMappingEditorContext::accept_action_drop() {
    if (!ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->InnerRect,
                                          ImGui::GetID("##action_drop"))) {
        return;
    }

    if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
        const std::string rel_path(static_cast<const char*>(payload->Data),
                                   static_cast<size_t>(payload->DataSize - 1));

        if (std::filesystem::path(rel_path).extension() == ".fjaction") {
            const auto def = parse_action_file(
                base_path::resolve_asset(rel_path, deps_.project_mgr->active()));

            if (def.name.empty()) {
                FJELL_CORE_WARN("Input mapping: {} has no action name", rel_path);
            } else {
                const bool already = std::any_of(
                    context_.bindings.begin(), context_.bindings.end(),
                    [&](const InputBinding& b) { return b.action == def.name; });

                if (!already) {
                    InputBinding binding;
                    binding.action = def.name;
                    context_.bindings.push_back(std::move(binding));

                    // A drop can name an action the picker never listed, because
                    // it scans one directory and the browser reaches anywhere.
                    if (std::none_of(discovered_actions_.begin(), discovered_actions_.end(),
                                     [&](const InputActionDef& a) { return a.name == def.name; })) {
                        discovered_actions_.push_back(def);
                        std::sort(discovered_actions_.begin(), discovered_actions_.end(),
                                  [](const InputActionDef& a, const InputActionDef& b) {
                                      return a.name < b.name;
                                  });
                    }
                }
            }
        }
    }

    ImGui::EndDragDropTarget();
}

void InputMappingEditorContext::draw_action_section(const std::string& action_name,
                                                     ActionType type) {
    ImGui::PushID(action_name.c_str());

    ImU32 type_color = action_type_color(type);

    // ── Action header row ─────────────────────────────────────────
    // Colored type strip on the left (aligns with the row, not a card).
    ImVec2 row_start = ImGui::GetCursorScreenPos();
    float row_h = ImGui::GetFrameHeight();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({row_start.x, row_start.y + 2.0F},
                      {row_start.x + 3.0F, row_start.y + row_h - 2.0F},
                      type_color, 1.5F);

    ImGui::Indent(10.0F);

    bool open = ImGui::TreeNodeEx("##action",
        ImGuiTreeNodeFlags_DefaultOpen |
        ImGuiTreeNodeFlags_AllowOverlap |
        ImGuiTreeNodeFlags_FramePadding |
        ImGuiTreeNodeFlags_SpanAvailWidth);

    ImGui::SameLine(0.0F, 4.0F);
    ImGui::AlignTextToFramePadding();
    bool bold_pushed = push_bold_font();
    ImGui::TextUnformatted(action_name.c_str());
    if (bold_pushed) ImGui::PopFont();

    // Type pill
    ImGui::SameLine(0.0F, 10.0F);
    ImGui::AlignTextToFramePadding();
    draw_pill(action_type_label(type), type_color);

    // Right-aligned remove
    {
        const char* x_label = "Remove";
        float w = ImGui::CalcTextSize(x_label).x + ImGui::GetStyle().FramePadding.x * 2.0F + 4.0F;
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(0.0F, std::max(8.0F, avail - w));

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::srgb(0.45F, 0.20F, 0.20F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  theme::srgb(0.55F, 0.24F, 0.24F));
        ImGui::PushStyleColor(ImGuiCol_Text,          theme::srgb(0.72F, 0.42F, 0.42F));
        if (ImGui::SmallButton(x_label)) {
            remove_action_name_ = action_name;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove action and all its bindings");
        }
        ImGui::PopStyleColor(4);
    }

    if (open) {
        ImGui::Dummy({0.0F, 4.0F});

        bool has_any = false;
        for (int i = 0; i < static_cast<int>(context_.bindings.size()); ++i) {
            if (context_.bindings[i].action != action_name) continue;
            has_any = true;

            ImGui::PushID(i);
            draw_binding_row(i);
            ImGui::PopID();
        }

        if (!has_any) {
            ImGui::Indent(16.0F);
            ImGui::TextDisabled("No bindings yet.");
            ImGui::Unindent(16.0F);
        }

        // ── Add binding row ───────────────────────────────────────
        ImGui::Dummy({0.0F, 2.0F});
        ImGui::Indent(16.0F);

        if (ImGui::SmallButton("+ Binding")) {
            InputBinding b;
            b.action = action_name;
            context_.bindings.push_back(std::move(b));
        }
        if (type == ActionType::axis2d) {
            ImGui::SameLine(0.0F, 8.0F);
            if (ImGui::SmallButton("+ Composite (WASD)")) {
                InputBinding b;
                b.action = action_name;
                b.up = InputSource::key_w;
                b.down = InputSource::key_s;
                b.left = InputSource::key_a;
                b.right = InputSource::key_d;
                context_.bindings.push_back(std::move(b));
            }
        }

        ImGui::Unindent(16.0F);
        ImGui::Dummy({0.0F, 2.0F});

        ImGui::TreePop();
    }

    ImGui::Unindent(10.0F);

    ImGui::PopID();
}

// ── Binding row ─────────────────────────────────────────────────────

void InputMappingEditorContext::draw_binding_row(int binding_idx) {
    auto& binding = context_.bindings[binding_idx];

    ImGui::Indent(16.0F);

    // Bullet on the left
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled({cursor.x - 8.0F, cursor.y + ImGui::GetFrameHeight() * 0.5F},
                        2.0F, col_text_muted());

    // ── Source(s) ─────────────────────────────────────────────────
    if (binding.is_composite()) {
        draw_source_button("UP", binding.up, binding_idx, RebindTarget::up);
        ImGui::SameLine(0.0F, 6.0F);
        draw_source_button("LF", binding.left, binding_idx, RebindTarget::left);
        ImGui::SameLine(0.0F, 6.0F);
        draw_source_button("RT", binding.right, binding_idx, RebindTarget::right);
        ImGui::SameLine(0.0F, 6.0F);
        draw_source_button("DN", binding.down, binding_idx, RebindTarget::down);

        ImGui::SameLine(0.0F, 10.0F);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("composite");
    } else {
        draw_source_button("", binding.source, binding_idx, RebindTarget::source);
    }

    // Right-aligned remove "x"
    float avail = ImGui::GetContentRegionAvail().x;
    float x_w = ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2.0F + 4.0F;
    ImGui::SameLine(0.0F, std::max(8.0F, avail - x_w));
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::srgb(0.40F, 0.20F, 0.20F));
    ImGui::PushStyleColor(ImGuiCol_Text,          theme::srgb(0.55F, 0.55F, 0.58F));
    if (ImGui::SmallButton("x")) {
        remove_binding_idx_ = binding_idx;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Remove binding");
    }
    ImGui::PopStyleColor(3);

    // ── Modifiers on a second indented line ───────────────────────
    ImGui::Indent(4.0F);
    draw_modifier_chips(binding_idx);
    ImGui::Unindent(4.0F);

    ImGui::Unindent(16.0F);
    ImGui::Dummy({0.0F, 4.0F});
}

bool InputMappingEditorContext::draw_source_button(const char* label, InputSource source,
                                                    int binding_idx, RebindTarget target) {
    bool is_this_rebinding = rebinding_ && rebind_binding_idx_ == binding_idx &&
                             rebind_target_ == target;

    SrcKind kind = source_kind(source);
    bool is_empty = (source == InputSource::none);

    char btn_label[160];
    if (is_this_rebinding) {
        std::snprintf(btn_label, sizeof(btn_label),
                      "%s%spress any key...##rebind_%s",
                      label[0] ? label : "",
                      label[0] ? "  " : "",
                      label);
    } else if (is_empty) {
        std::snprintf(btn_label, sizeof(btn_label),
                      "%s%s(unbound)##empty_%s",
                      label[0] ? label : "",
                      label[0] ? "  " : "",
                      label);
    } else {
        auto display = input_source_display_name(source);
        const char* tag = source_kind_tag(kind);
        if (label[0]) {
            std::snprintf(btn_label, sizeof(btn_label), "%s  %s %s##b_%s",
                          label, tag, display.c_str(), label);
        } else {
            std::snprintf(btn_label, sizeof(btn_label), "%s  %s##b",
                          tag, display.c_str());
        }
    }

    if (is_this_rebinding) {
        ImGui::PushStyleColor(ImGuiCol_Button,        theme::accent());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::accent_hov());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  theme::accent_hov());
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4{1.0F, 1.0F, 1.0F, 1.0F});
    } else if (is_empty) {
        ImGui::PushStyleColor(ImGuiCol_Button,        theme::srgb(0.090F, 0.094F, 0.106F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::srgb(0.169F, 0.176F, 0.192F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  theme::srgb(0.067F, 0.071F, 0.078F));
        ImGui::PushStyleColor(ImGuiCol_Text,          theme::srgb(0.55F, 0.56F, 0.60F));
    } else {
        // Use engine's frame bg for non-empty: stays in palette, reads as a chip.
        ImGui::PushStyleColor(ImGuiCol_Button,        theme::srgb(0.118F, 0.122F, 0.133F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::srgb(0.169F, 0.176F, 0.192F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  theme::srgb(0.090F, 0.094F, 0.106F));
        ImGui::PushStyleColor(ImGuiCol_Text,          theme::srgb(0.898F, 0.902F, 0.918F));
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10.0F, 5.0F});

    ImVec2 btn_pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Button(btn_label);
    ImVec2 btn_size = ImGui::GetItemRectSize();

    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(4);

    // Category color stripe on the left edge of the button (bound only)
    if (!is_empty && !is_this_rebinding) {
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled({btn_pos.x, btn_pos.y + 2.0F},
                          {btn_pos.x + 3.0F, btn_pos.y + btn_size.y - 2.0F},
                          source_kind_color(kind));
    }

    if (ImGui::IsItemHovered() && !is_this_rebinding) {
        ImGui::SetTooltip("%s — click to rebind",
                          is_empty ? "no source bound"
                                   : input_source_display_name(source).c_str());
    }

    if (clicked && !rebinding_) {
        rebinding_ = true;
        rebind_binding_idx_ = binding_idx;
        rebind_target_ = target;
    }

    // The picker beside the listener, for a source there is no way to press
    // here: an axis, or a pad button with no pad plugged in.
    ImGui::SameLine(0.0F, 4.0F);
    ImGui::PushID(binding_idx);
    ImGui::PushID(static_cast<int>(target));

    // grouped_picker tracks which picker is open by this string alone — it
    // does not consult ImGui's ID stack — so every field needs its own, or
    // they all read as open at once and the first one drawn takes the pick.
    char picker_id[48];
    std::snprintf(picker_id, sizeof(picker_id), "src_%d_%d", binding_idx,
                  static_cast<int>(target));

    ImGui::PushStyleColor(ImGuiCol_Button,        theme::srgb(0.118F, 0.122F, 0.133F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::srgb(0.169F, 0.176F, 0.192F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  theme::srgb(0.090F, 0.094F, 0.106F));
    ImGui::PushStyleColor(ImGuiCol_Text,          theme::srgb(0.55F, 0.56F, 0.60F));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{5.0F, 5.0F});
    const bool open_picker = ImGui::Button("v");
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(4);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pick a source from a list");
    }
    if (open_picker) {
        // Arming both at once would have the listener take whatever key the
        // player presses to drive the list.
        rebinding_ = false;
        grouped_picker::open(picker_id);
    }

    // Drawn right after the button it hangs from, as grouped_picker requires.
    // Gated on is_open() because the argument is built before the call: every
    // source button would otherwise rebuild the whole source list every
    // frame, and a row carries several strings.
    static constexpr grouped_picker::Config PICKER_CONFIG{
        .search_hint = "Search sources",
        .empty_message = "No matching source.",
        .loading_message = "",
        .loading = false,
        .hide_group_headers = false,
        .size = {300.0F, 420.0F},
    };
    if (std::string picked;
        grouped_picker::is_open(picker_id)
        && grouped_picker::draw(picker_id, build_source_groups(), PICKER_CONFIG,
                                picked)) {
        if (const InputSource chosen = input_source_from_string(picked);
            chosen != InputSource::none && binding_idx >= 0
            && binding_idx < static_cast<int>(context_.bindings.size())) {
            auto& bound = context_.bindings[binding_idx];
            switch (target) {
                case RebindTarget::source: bound.source = chosen; break;
                case RebindTarget::up:     bound.up = chosen; break;
                case RebindTarget::down:   bound.down = chosen; break;
                case RebindTarget::left:   bound.left = chosen; break;
                case RebindTarget::right:  bound.right = chosen; break;
            }
        }
    }

    ImGui::PopID();
    ImGui::PopID();

    return clicked;
}

void InputMappingEditorContext::draw_modifier_chips(int binding_idx) {
    auto& binding = context_.bindings[binding_idx];

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Modifiers:");
    ImGui::SameLine(0.0F, 8.0F);

    if (binding.modifiers.empty()) {
        if (ImGui::SmallButton("+ add")) {
            binding.modifiers.push_back({ModifierType::scale, 1.0F});
        }
        return;
    }

    for (int mi = 0; mi < static_cast<int>(binding.modifiers.size()); ++mi) {
        auto& mod = binding.modifiers[mi];
        ImGui::PushID(mi + 1000);

        char chip[80];
        if (mod.type == ModifierType::negate || mod.type == ModifierType::swizzle) {
            std::snprintf(chip, sizeof(chip), "%s", modifier_type_label(mod.type));
        } else {
            std::snprintf(chip, sizeof(chip), "%s  %.2f", modifier_type_label(mod.type), mod.value);
        }

        // Stay in palette: amber-tinted chip, dimmed.
        ImGui::PushStyleColor(ImGuiCol_Button,        theme::srgb(0.169F, 0.176F, 0.192F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::srgb(0.220F, 0.227F, 0.251F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  theme::srgb(0.090F, 0.094F, 0.106F));
        ImGui::PushStyleColor(ImGuiCol_Text,          theme::accent());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2{8.0F, 3.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0F);

        if (ImGui::SmallButton(chip)) {
            ImGui::OpenPopup("edit_mod");
        }

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to edit modifier");
        }

        if (ImGui::BeginPopup("edit_mod")) {
            ImGui::TextUnformatted("Edit Modifier");
            ImGui::Separator();
            ImGui::Dummy({0.0F, 4.0F});

            int type_idx = static_cast<int>(mod.type);
            const char* type_names[] = {"Scale", "Deadzone", "Negate", "Swizzle"};
            ImGui::SetNextItemWidth(160.0F);
            if (ImGui::Combo("Type", &type_idx, type_names, 4)) {
                mod.type = static_cast<ModifierType>(type_idx);
            }
            if (mod.type == ModifierType::scale || mod.type == ModifierType::deadzone) {
                ImGui::SetNextItemWidth(160.0F);
                (void)ImGui::DragFloat("Value", &mod.value, 0.01F, 0.0F, 10.0F, "%.3f");
            }
            ImGui::Dummy({0.0F, 4.0F});
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Button,        theme::srgb(0.42F, 0.20F, 0.20F));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::srgb(0.55F, 0.26F, 0.26F));
            if (ImGui::Button("Remove modifier", ImVec2{160.0F, 0.0F})) {
                binding.modifiers.erase(binding.modifiers.begin() + mi);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::EndPopup();
        }

        ImGui::SameLine(0.0F, 4.0F);
        ImGui::PopID();
    }

    // Trailing "+" to add another modifier
    if (ImGui::SmallButton("+")) {
        binding.modifiers.push_back({ModifierType::scale, 1.0F});
    }
}

// ── Add Action popup ────────────────────────────────────────────────

void InputMappingEditorContext::draw_add_action_popup() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{14.0F, 12.0F});
    if (!ImGui::BeginPopup("add_action_popup")) {
        ImGui::PopStyleVar();
        return;
    }

    ImGui::TextUnformatted("Add Action to Context");
    ImGui::Separator();
    ImGui::Dummy({0.0F, 4.0F});

    ImGui::SetNextItemWidth(280.0F);
    ui::search_field("##filter", action_filter_);

    ImGui::Dummy({0.0F, 4.0F});

    std::vector<std::string> existing;
    for (const auto& b : context_.bindings) {
        if (std::find(existing.begin(), existing.end(), b.action) == existing.end()) {
            existing.push_back(b.action);
        }
    }

    auto passes_filter = [&](const std::string& name) { return ui::matches(name, action_filter_); };

    ImGui::BeginChild("action_list", ImVec2{320.0F, 280.0F});

    auto draw_group = [&](const char* heading, ActionType filter_type) {
        bool any = false;
        for (const auto& a : discovered_actions_) {
            if (a.type != filter_type) continue;
            if (!passes_filter(a.name)) continue;
            any = true; break;
        }
        if (!any) return;

        ImGui::Dummy({0.0F, 2.0F});
        ImGui::TextDisabled("%s", heading);
        ImGui::Separator();
        ImGui::Dummy({0.0F, 2.0F});

        for (const auto& action : discovered_actions_) {
            if (action.type != filter_type) continue;
            if (!passes_filter(action.name)) continue;

            bool already = std::find(existing.begin(), existing.end(), action.name) != existing.end();

            ImGui::PushID(action.name.c_str());
            if (already) {
                ImGui::BeginDisabled();
                char label[256];
                std::snprintf(label, sizeof(label), "%s   (already added)", action.name.c_str());
                ImGui::Selectable(label);
                ImGui::EndDisabled();
            } else {
                ImVec2 row_start = ImGui::GetCursorScreenPos();
                float row_w = ImGui::GetContentRegionAvail().x;
                float row_h = ImGui::GetFrameHeight();

                bool clicked = ImGui::Selectable("##row", false, 0, ImVec2{row_w, row_h});

                auto* dl = ImGui::GetWindowDrawList();
                dl->AddText({row_start.x + 6.0F, row_start.y + 3.0F},
                            to_imu32(theme::srgb(0.898F, 0.902F, 0.918F)),
                            action.name.c_str());

                const char* tag = action_type_short(action.type);
                ImVec2 ts = ImGui::CalcTextSize(tag);
                ImVec2 pill_pad{6.0F, 1.0F};
                ImVec2 pill_p0{row_start.x + row_w - ts.x - pill_pad.x * 2.0F - 6.0F,
                               row_start.y + (row_h - ts.y - pill_pad.y * 2.0F) * 0.5F};
                ImVec2 pill_p1{pill_p0.x + ts.x + pill_pad.x * 2.0F,
                               pill_p0.y + ts.y + pill_pad.y * 2.0F};
                dl->AddRectFilled(pill_p0, pill_p1, action_type_color(action.type), 3.0F);
                dl->AddText({pill_p0.x + pill_pad.x, pill_p0.y + pill_pad.y},
                            IM_COL32(20, 22, 26, 255), tag);

                if (clicked) {
                    InputBinding b;
                    b.action = action.name;
                    context_.bindings.push_back(std::move(b));
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::PopID();
        }
    };

    draw_group("BUTTONS", ActionType::button);
    draw_group("1D AXES", ActionType::axis1d);
    draw_group("2D AXES", ActionType::axis2d);

    bool any_match = false;
    for (const auto& a : discovered_actions_) {
        if (passes_filter(a.name)) { any_match = true; break; }
    }
    if (!any_match) {
        ImGui::Dummy({0.0F, 8.0F});
        ImGui::TextDisabled("No actions match filter.");
    }

    ImGui::EndChild();
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

} // namespace fjell
