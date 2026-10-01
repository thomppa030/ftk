#pragma once

namespace fjell {

/// How this machine likes the editor's panels shown: saved with the editor
/// layout in ImGui's ini file, never part of a project.
struct EditorViewSettings {
    /// The Hierarchy tree's text size, in px.
    int hierarchy_text_size{14};
};

inline constexpr int HIERARCHY_TEXT_MIN = 11;
inline constexpr int HIERARCHY_TEXT_MAX = 20;

/// The settings of the current ImGui context's editor.
[[nodiscard]] EditorViewSettings& editor_view_settings();

/// Registers the ini section they are kept in. Once per ImGui context, after
/// ImGui::CreateContext() and before the ini is first read.
void register_editor_view_settings();

/// Marks the ini to be written after a setting changed.
void editor_view_settings_changed();

} // namespace fjell
