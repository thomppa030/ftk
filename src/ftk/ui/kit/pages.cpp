#include "ftk/ui/kit/pages.hpp"

#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <algorithm>

namespace fjell::ui {

namespace {

constexpr float ENTRY_HEIGHT = 26.0f;
constexpr float TITLE_SIZE = 16.0f;

} // namespace

PagedWindow::PagedWindow(const char* title, bool* open, ImVec2 size) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    // The list and the page run to the window's edges; each pads itself.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    open_ = ImGui::Begin(title, open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse
                                          | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
}

PagedWindow::~PagedWindow() {
    ImGui::End();
}

PageBody::PageBody(const char* id, float reserve_bottom) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {theme::GAP_L, theme::GAP_M + theme::GAP_XS});
    const float height = std::max(ImGui::GetContentRegionAvail().y - reserve_bottom, 1.0f);
    open_ = ImGui::BeginChild(id, {0.0f, height}, ImGuiChildFlags_AlwaysUseWindowPadding);
}

PageBody::~PageBody() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

PageList::PageList(const char* id, float reserve_bottom) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::surface_sunken());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {theme::GAP_M, theme::GAP_M});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {theme::GAP_S, theme::GAP_XS});
    const float height = std::max(ImGui::GetContentRegionAvail().y - reserve_bottom, 1.0f);
    open_ = ImGui::BeginChild(id, {PAGE_LIST_WIDTH, height}, ImGuiChildFlags_AlwaysUseWindowPadding);
}

PageList::~PageList() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

bool page_entry(const char* icon, const char* label, bool selected, bool dimmed) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(label);
    const bool clicked = ImGui::InvisibleButton("##page", {std::max(width, 1.0f), ENTRY_HEIGHT});
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 max = ImGui::GetItemRectMax();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding;
    if (selected) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::selection()), rounding);
    } else if (hovered) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::surface_hover()), rounding);
    }

    const ImVec4 ink = dimmed ? theme::text_disabled()
                     : selected || hovered ? theme::text() : theme::text_secondary();
    const ImVec4 icon_ink = dimmed ? theme::text_disabled() : selected ? theme::accent() : ink;
    const float y = min.y + (ENTRY_HEIGHT - ImGui::GetTextLineHeight()) * 0.5f;
    const float x = min.x + theme::GAP_M;
    dl->AddText({x, y}, ImGui::GetColorU32(icon_ink), icon);
    dl->AddText({x + ImGui::CalcTextSize(icon).x + theme::GAP_M, y}, ImGui::GetColorU32(ink), label);
    return clicked;
}

void page_title(const char* icon, const char* label) {
    ImGui::PushFont(theme::bold_font(), TITLE_SIZE);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::accent());
    ImGui::TextUnformatted(icon);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, theme::GAP_M);
    ImGui::TextUnformatted(label);
    ImGui::PopFont();
    ImGui::Dummy({0.0f, theme::GAP_S});
}

} // namespace fjell::ui
