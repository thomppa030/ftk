#include "ui/kit/grouped_picker.hpp"
#include "ui/kit/search.hpp"
#include "ui/kit/section.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace fjell::ui::grouped_picker {
namespace {

constexpr float PREVIEW_SIZE = 32.0F;
// The column a row's category dot sits in, and the dot.
constexpr float DOT_COLUMN = 16.0F;
constexpr float DOT_DIAMETER = 7.0F;

// Only one picker is open at a time, the same way only one popup is.
// Pickers are told apart by their callers' ImGui ids: the same widget id
// under two different pushed ids is two pickers. 0 is none.
ImGuiID g_open_for = 0;      // the picker that is open
ImGuiID g_pending_open = 0;  // the picker asked to open on the next draw

ImGuiID picker_id(const char* widget_id) { return ImGui::GetID(widget_id != nullptr ? widget_id : ""); }
std::string g_search;
// The item the arrow keys have reached, counted over the pickable items
// shown, and the search it was counted under.
int g_highlight = 0;
std::string g_highlight_search;
// How many pickable items the last draw showed, which bounds the highlight:
// what a folded group hides isn't known until it is drawn.
int g_shown_pickable = 0;
// Set when the arrow keys moved the highlight, to bring it into view once.
bool g_scroll_to_highlight = false;

bool matches_search(const Item& item, std::string_view needle) {
    return ui::matches(item.label, needle) || ui::matches(item.sublabel, needle)
        || ui::matches(item.search_text, needle);
}

/// A short word in a coloured pill, for a row's category.
void draw_tag(const std::string& text, ImU32 color) {
    const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pad{6.0F, 2.0F};
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1{p0.x + text_size.x + pad.x * 2.0F,
                    p0.y + text_size.y + pad.y * 2.0F};

    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(p0, p1, color, 4.0F);
    draw_list->AddText({p0.x + pad.x, p0.y + pad.y}, ImGui::GetColorU32(theme::text_on_accent()),
                       text.c_str());
    ImGui::Dummy({p1.x - p0.x, p1.y - p0.y});
}

/// Draw one row: category dot, preview or tag, then the label over its
/// sublabel, and the detail word at the right end. Returns true when it was
/// clicked.
bool draw_row(const Item& item, bool highlighted, bool dot_column, std::string_view query) {
    const bool two_line = !item.sublabel.empty();
    const float text_height = ImGui::GetTextLineHeight() * (two_line ? 2.0F : 1.0F);
    const float row_height = item.preview != 0
        ? std::max(PREVIEW_SIZE, text_height)
        : text_height;

    ImGui::BeginDisabled(!item.enabled);
    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::Selectable(("##" + item.value).c_str(), highlighted,
                                           ImGuiSelectableFlags_None,
                                           {0.0F, row_height});
    if (!item.disabled_reason.empty()) ImGui::SetItemTooltip("%s", item.disabled_reason.c_str());
    if (highlighted && g_scroll_to_highlight) {
        ImGui::SetScrollHereY();
        g_scroll_to_highlight = false;
    }
    const float row_right = ImGui::GetItemRectMax().x;
    ImGui::SameLine(0.0F, 0.0F);

    ImGui::BeginGroup();
    if (dot_column) {
        if (item.category) {
            ImGui::GetWindowDrawList()->AddCircleFilled(
                {row_min.x + DOT_COLUMN * 0.5F, row_min.y + row_height * 0.5F}, DOT_DIAMETER * 0.5F,
                ImGui::ColorConvertFloat4ToU32(theme::category(*item.category)));
        }
        ImGui::Dummy({DOT_COLUMN, row_height});
        ImGui::SameLine();
    }
    if (item.preview != 0) {
        ImGui::Image(item.preview, {PREVIEW_SIZE, PREVIEW_SIZE});
        ImGui::SameLine();
    } else if (!item.tag.empty()) {
        draw_tag(item.tag, item.tag_color);
        ImGui::SameLine();
    }

    ImGui::BeginGroup();
    ui::highlighted_text(item.label, query);
    if (two_line) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(item.sublabel.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
    if (!item.detail.empty()) {
        const float width = ImGui::CalcTextSize(item.detail.c_str()).x;
        ImGui::SameLine(row_right - ImGui::GetWindowPos().x + ImGui::GetScrollX() - width
                        - ImGui::GetStyle().ItemSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
        ImGui::TextUnformatted(item.detail.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
    ImGui::EndDisabled();

    return clicked && item.enabled;
}

} // namespace

void open(const char* widget_id) {
    g_pending_open = picker_id(widget_id);
    g_search.clear();
    g_highlight = 0;
    g_highlight_search.clear();
    g_shown_pickable = 0;
}

bool is_open(const char* widget_id) {
    const ImGuiID id = picker_id(widget_id);
    return g_open_for == id || g_pending_open == id;
}

void close() {
    g_open_for = 0;
    g_pending_open = 0;
}

bool draw(const char* widget_id, std::span<const Group> groups,
          const Config& config, std::string& picked) {
    const ImGuiID id = picker_id(widget_id);

    if (g_pending_open != 0 && g_pending_open == id) {
        g_pending_open = 0;
        g_open_for = id;
        ImGui::OpenPopup("##grouped_picker");
    }

    if (g_open_for != id) return false;

    ImGui::SetNextWindowSize(config.size, ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("##grouped_picker")) {
        // Dismissed by clicking away or pressing Escape.
        g_open_for = 0;
        return false;
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ui::search_field("##search", g_search, config.search_hint);

    bool chose = false;

    // The items the arrow keys walk: those shown and pickable, in order.
    const std::string_view needle{g_search};
    if (g_highlight_search != needle) {
        g_highlight = 0;
        g_highlight_search = needle;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        ++g_highlight;
        g_scroll_to_highlight = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        --g_highlight;
        g_scroll_to_highlight = true;
    }
    g_highlight = g_shown_pickable > 0 ? std::clamp(g_highlight, 0, g_shown_pickable - 1) : 0;
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

    const bool dot_column = std::ranges::any_of(groups, [](const Group& g) {
        return std::ranges::any_of(g.items, [](const Item& i) { return i.category.has_value(); });
    });

    if (ImGui::BeginChild("##list", {0.0F, 0.0F})) {
        bool any_shown = false;
        int index = 0;

        for (const auto& group : groups) {
            // A group whose every item is filtered out takes its header with
            // it, so a search does not leave a page of empty headings.
            const bool has_match = std::ranges::any_of(
                group.items, [&](const Item& i) { return matches_search(i, needle); });
            if (!has_match) continue;
            any_shown = true;

            // An item may sit in two groups ("Recently added" and its own),
            // so each group is its own ID scope.
            ImGui::PushID(group.label.c_str());
            bool body_open = true;
            if (!config.hide_group_headers) {
                body_open = group.category
                    ? ui::subheading_foldable(group.label.c_str(), *group.category, group.open)
                    : ui::subheading_foldable(group.label.c_str(), group.open);
            }
            if (!body_open) {
                ImGui::PopID();
                continue;
            }
            for (const auto& item : group.items) {
                if (!matches_search(item, needle)) continue;
                const bool highlighted = item.enabled && index == g_highlight;
                if (item.enabled) ++index;
                ImGui::PushID(item.value.c_str());
                if (draw_row(item, highlighted, dot_column, needle) || (highlighted && enter)) {
                    picked = item.value;
                    chose = true;
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }

        g_shown_pickable = index;
        if (!any_shown) {
            ImGui::TextDisabled("%s", config.loading ? config.loading_message
                                                     : config.empty_message);
        }
    }
    ImGui::EndChild();

    if (chose) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();

    if (chose) g_open_for = 0;
    return chose;
}

} // namespace fjell::ui::grouped_picker
