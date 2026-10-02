#include "ftk/ui/kit/selectable.hpp"

#include <imgui.h>

namespace ftk::ui {

bool selectable(const char* label, bool selected, const SelectableOptions& options) {
    ImGuiSelectableFlags flags = ImGuiSelectableFlags_None;
    if (options.span_columns) flags |= ImGuiSelectableFlags_SpanAllColumns;
    if (options.double_click) flags |= ImGuiSelectableFlags_AllowDoubleClick;
    if (options.overlap) flags |= ImGuiSelectableFlags_AllowOverlap;
    return ImGui::Selectable(label, selected, flags, options.size);
}

} // namespace ftk::ui
