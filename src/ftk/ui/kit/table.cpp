#include "ftk/ui/kit/table.hpp"

#include <imgui.h>

namespace ftk::ui {

Table::Table(const char* id, std::span<const Column> columns, TableOptions options) {
    ImGuiTableFlags flags = ImGuiTableFlags_None;
    if (options.resizable) flags |= ImGuiTableFlags_Resizable;
    if (options.sortable) flags |= ImGuiTableFlags_Sortable;
    if (options.scroll) flags |= ImGuiTableFlags_ScrollY;
    if (options.column_lines) flags |= ImGuiTableFlags_BordersInnerV;
    if (options.proportional) flags |= ImGuiTableFlags_SizingStretchProp;
    open_ = ImGui::BeginTable(id, static_cast<int>(columns.size()), flags);
    if (!open_) return;
    for (const Column& column : columns) {
        ImGuiTableColumnFlags column_flags = column.sizing == ColumnWidth::Fixed ? ImGuiTableColumnFlags_WidthFixed
                                                                                : ImGuiTableColumnFlags_WidthStretch;
        if (column.default_sort) column_flags |= ImGuiTableColumnFlags_DefaultSort;
        ImGui::TableSetupColumn(column.name, column_flags, column.width);
    }
    if (options.scroll) ImGui::TableSetupScrollFreeze(0, 1);
}

Table::~Table() {
    if (open_) ImGui::EndTable();
}

void table_headers() {
    ImGui::TableHeadersRow();
}

void table_next_row() {
    ImGui::TableNextRow();
}

bool table_next_column() {
    return ImGui::TableNextColumn();
}

std::optional<TableSort> table_sort_changed() {
    ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
    if (specs == nullptr || !specs->SpecsDirty || specs->SpecsCount == 0) return std::nullopt;
    specs->SpecsDirty = false;
    return TableSort{.column = specs->Specs[0].ColumnIndex,
                     .ascending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending};
}

} // namespace ftk::ui
