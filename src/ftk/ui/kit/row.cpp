#include "ftk/ui/kit/row.hpp"

#include "ftk/ui/kit/edit_record.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui_internal.h>

#include <string>
#include <utility>

namespace fjell::ui {

namespace {

// The help icon after a label: dim until the mouse is on it, the
// explanation in a tooltip wrapped at the tooltip width.
void help_icon(const char* help) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::CalcTextSize(icon::help);
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
        && ImGui::IsMouseHoveringRect(p, {p.x + size.x, p.y + ImGui::GetFrameHeight()});
    ImGui::PushStyleColor(ImGuiCol_Text, hovered ? theme::text() : theme::text_disabled());
    ImGui::TextUnformatted(icon::help);
    ImGui::PopStyleColor();
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(theme::TOOLTIP_WRAP);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// The search a RowFilter puts over the rows drawn while it lives.
struct Filter {
    bool active{false};
    std::string query;
    int matches{0};
};

Filter& filter() {
    static Filter state;
    return state;
}

// What mark_next_row() asked of the next row: nothing, room only, or the dot.
enum class Mark { None, Room, Dot };
Mark& next_mark() {
    static Mark mark = Mark::None;
    return mark;
}

// The dot, or the room it takes, before a label.
void draw_mark(Mark mark) {
    constexpr float DOT = 6.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (mark == Mark::Dot) {
        const float cy = p.y + ImGui::GetFrameHeight() * 0.5f;
        ImGui::GetWindowDrawList()->AddCircleFilled({p.x + DOT * 0.5f, cy}, DOT * 0.5f,
                                                    ImGui::GetColorU32(theme::accent()));
    }
    ImGui::Dummy({DOT, ImGui::GetFrameHeight()});
    ImGui::SameLine(0.0f, theme::GAP_S + theme::GAP_XS);
    ImGui::AlignTextToFramePadding();
}

} // namespace

RowFilter::RowFilter(std::string_view query) {
    filter() = {.active = !query.empty(), .query = std::string(query), .matches = 0};
}

RowFilter::~RowFilter() {
    filter() = {};
}

int RowFilter::matches() const {
    return filter().matches;
}

bool detail::filtering() {
    return filter().active;
}

PropertyTable::PropertyTable(const char* id) {
    const float label_width = theme::label_column(ImGui::GetContentRegionAvail().x);
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp
                                | ImGuiTableFlags_NoSavedSettings
                                | ImGuiTableFlags_PadOuterX;
    open_ = ImGui::BeginTable(id, 2, flags);
    if (open_) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, label_width);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    }
}

PropertyTable::~PropertyTable() {
    if (open_) {
        ImGui::EndTable();
    }
    detail::set_edit_label(nullptr);
}

bool detail::begin_row(const char* label, const char* help) {
    // Taken here so a row a search leaves out doesn't hand its mark on.
    const Mark mark = std::exchange(next_mark(), Mark::None);
    Filter& search = filter();
    if (search.active && !matches(label, search.query)) return false;
    set_edit_label(label);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    if (mark != Mark::None) draw_mark(mark);
    if (search.active) {
        ++search.matches;
        highlighted_text(label, search.query);
    } else {
        ImGui::TextUnformatted(label);
    }
    if (help != nullptr) {
        ImGui::SameLine(0.0f, theme::GAP_S);
        help_icon(help);
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    return true;
}

void mark_next_row(bool set_here) {
    next_mark() = set_here ? Mark::Dot : Mark::Room;
}

void hint(const char* text) {
    // A hint belongs to the row above, which a search may have left out.
    if (detail::filtering()) return;
    ImGui::PushFont(nullptr, theme::SMALL_TEXT);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    if (ImGuiTable* table = ImGui::GetCurrentTable()) {
        // A row of its own that runs under both columns, from the label's
        // edge to the value's, rather than squeezed into the label column.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const ImGuiWindow* window = ImGui::GetCurrentWindow();
        const float left = ImGui::GetCursorScreenPos().x;
        const float right = table->WorkRect.Max.x;
        ImGui::PushClipRect({left, table->InnerClipRect.Min.y},
                            {right, table->InnerClipRect.Max.y}, false);
        ImGui::PushTextWrapPos(right - window->Pos.x + window->Scroll.x);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopClipRect();
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

} // namespace fjell::ui
