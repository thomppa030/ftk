#pragma once

#include "ui/editor_context.hpp"
#include "core/input_action.hpp"
#include "core/input_mapping_context.hpp"
#include "core/input_source.hpp"

#include <string>
#include <vector>

namespace fjell {

class Input;
class InputActionMap;
class ProjectManager;
class VulkanContext;

struct InputMappingEditorDeps {
    VulkanContext* vulkan_context;
    Input* input;
    ProjectManager* project_mgr;
    InputActionMap* input_action_map;
};

class InputMappingEditorContext final : public EditorContext {
public:
    explicit InputMappingEditorContext(InputMappingEditorDeps deps);

    [[nodiscard]] const char* name() const override { return name_.c_str(); }
    [[nodiscard]] const char* context_type() const override { return "input_mapping"; }
    [[nodiscard]] std::vector<std::string> docked_window_names() const override;
    [[nodiscard]] std::string asset_path() const override { return fjinput_path_; }

    void init() override;
    void shutdown() override;

    void draw(float dt) override;
    void setup_dockspace(ImGuiID main_area) override;
    bool handle_shortcuts() override;
    bool save() override;
    [[nodiscard]] bool has_unsaved_changes() const override { return dirty_; }

    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests() override { return {}; }
    void apply_resize(VulkanContext&) override {}

    void open_context(const std::string& fjinput_path);

private:
    enum class RebindTarget { source, up, down, left, right };

    void draw_mappings();
    void draw_action_section(const std::string& action_name, ActionType type);
    void draw_binding_row(int binding_idx);
    bool draw_source_button(const char* label, InputSource source, int binding_idx,
                            RebindTarget target);
    void draw_modifier_chips(int binding_idx);
    void draw_add_action_popup();
    void accept_action_drop();

    void scan_actions();

    /// The key, mouse button or pad button the player is pressing to claim the
    /// armed rebind, or `none` while they have not pressed anything.
    ///
    /// Reads raw input deliberately: the editor runs with the game unfocused,
    /// and the gated queries report nothing there.
    ///
    /// Axis sources (mouse motion, scroll, sticks, triggers) cannot be offered
    /// here — there is no press to wait for, and a gesture does not say which
    /// of them was meant. They are picked explicitly instead.
    [[nodiscard]] InputSource poll_rebind_source() const;

    InputMappingEditorDeps deps_;
    std::string name_{"Input Mapping"};

    std::string fjinput_path_;
    std::string fjinput_abs_path_;
    InputMappingContext context_;
    bool dirty_{false};

    // Discovered actions for the "Add Action" picker
    std::vector<InputActionDef> discovered_actions_;

    // Rebind state
    bool rebinding_{false};
    int rebind_binding_idx_{-1};
    RebindTarget rebind_target_{RebindTarget::source};

    // "Add Action" popup filter
    std::string action_filter_;

    // Deferred removal
    int remove_binding_idx_{-1};
    std::string remove_action_name_;
};

} // namespace fjell
