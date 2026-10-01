#include "ftk/app/editor_view_settings.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fjell {

namespace {

// In the ini as
//     [FjellView][Editor]
//     HierarchyTextSize=15
constexpr const char* SECTION = "FjellView";
constexpr const char* ENTRY = "Editor";

void* read_open(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    return std::strcmp(name, ENTRY) == 0 ? &editor_view_settings() : nullptr;
}

void read_line(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    auto* settings = static_cast<EditorViewSettings*>(entry);
    int size = 0;
    if (std::sscanf(line, "HierarchyTextSize=%d", &size) == 1) {
        settings->hierarchy_text_size = std::clamp(size, HIERARCHY_TEXT_MIN, HIERARCHY_TEXT_MAX);
    }
}

void write_all(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out) {
    const EditorViewSettings& settings = editor_view_settings();
    out->appendf("[%s][%s]\n", handler->TypeName, ENTRY);
    out->appendf("HierarchyTextSize=%d\n", settings.hierarchy_text_size);
    out->append("\n");
}

} // namespace

EditorViewSettings& editor_view_settings() {
    static EditorViewSettings settings;
    return settings;
}

void register_editor_view_settings() {
    if (ImGui::FindSettingsHandler(SECTION) != nullptr) return;
    ImGuiSettingsHandler handler;
    handler.TypeName = SECTION;
    handler.TypeHash = ImHashStr(SECTION);
    handler.ReadOpenFn = read_open;
    handler.ReadLineFn = read_line;
    handler.WriteAllFn = write_all;
    ImGui::AddSettingsHandler(&handler);
}

void editor_view_settings_changed() {
    ImGui::MarkIniSettingsDirty();
}

} // namespace fjell
