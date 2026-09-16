#include "ui/grouped_picker.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace fjell::grouped_picker {
namespace {

constexpr float PREVIEW_SIZE = 32.0F;

// Only one picker is open at a time, the same way only one popup is.
std::string g_open_for;      // widget id whose picker is open
std::string g_pending_open;  // widget id asked to open on the next draw
std::array<char, 128> g_search{};

/// Case-insensitive substring test, so typing "brick" finds "Brick_01".
bool contains_fold(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    auto found = std::ranges::search(haystack, needle, [&](char a, char b) {
        return lower(a) == lower(b);
    });
    return !found.empty();
}

bool matches_search(const Item& item, std::string_view needle) {
    if (needle.empty()) return true;
    return contains_fold(item.label, needle) ||
           contains_fold(item.sublabel, needle) ||
           contains_fold(item.search_text, needle);
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
    draw_list->AddText({p0.x + pad.x, p0.y + pad.y}, IM_COL32(20, 22, 26, 255),
                       text.c_str());
    ImGui::Dummy({p1.x - p0.x, p1.y - p0.y});
}

/// Draw one row: preview or tag, then the label over its sublabel. Returns
/// true when it was clicked.
bool draw_row(const Item& item) {
    const bool two_line = !item.sublabel.empty();
    const float text_height = ImGui::GetTextLineHeight() * (two_line ? 2.0F : 1.0F);
    const float row_height = item.preview != 0
        ? std::max(PREVIEW_SIZE, text_height)
        : text_height;

    const bool clicked = ImGui::Selectable(("##" + item.value).c_str(), false,
                                           ImGuiSelectableFlags_None,
                                           {0.0F, row_height});
    ImGui::SameLine(0.0F, 0.0F);

    ImGui::BeginGroup();
    if (item.preview != 0) {
        ImGui::Image(item.preview, {PREVIEW_SIZE, PREVIEW_SIZE});
        ImGui::SameLine();
    } else if (!item.tag.empty()) {
        draw_tag(item.tag, item.tag_color);
        ImGui::SameLine();
    }

    ImGui::BeginGroup();
    ImGui::TextUnformatted(item.label.c_str());
    if (two_line) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(item.sublabel.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
    ImGui::EndGroup();

    return clicked;
}

} // namespace

void open(const char* widget_id) {
    g_pending_open = widget_id != nullptr ? widget_id : "";
    g_search.fill('\0');
}

bool is_open(const char* widget_id) {
    const std::string id = widget_id != nullptr ? widget_id : "";
    return g_open_for == id || g_pending_open == id;
}

void close() {
    g_open_for.clear();
    g_pending_open.clear();
}

bool draw(const char* widget_id, std::span<const Group> groups,
          const Config& config, std::string& picked) {
    const std::string id = widget_id != nullptr ? widget_id : "";

    if (!g_pending_open.empty() && g_pending_open == id) {
        g_pending_open.clear();
        g_open_for = id;
        ImGui::OpenPopup("##grouped_picker");
    }

    if (g_open_for != id) return false;

    ImGui::SetNextWindowSize(config.size, ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("##grouped_picker")) {
        // Dismissed by clicking away or pressing Escape.
        g_open_for.clear();
        return false;
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##search", config.search_hint,
                             g_search.data(), g_search.size());
    ImGui::Separator();

    bool chose = false;

    if (ImGui::BeginChild("##list", {0.0F, 0.0F})) {
        const std::string_view needle{g_search.data()};
        bool any_shown = false;

        for (const auto& group : groups) {
            // A group whose every item is filtered out takes its header with
            // it, so a search does not leave a page of empty headings.
            const bool has_match = std::ranges::any_of(
                group.items, [&](const Item& i) { return matches_search(i, needle); });
            if (!has_match) continue;
            any_shown = true;

            bool body_open = true;
            if (!config.hide_group_headers) {
                ImGui::SetNextItemOpen(group.open, ImGuiCond_Appearing);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::accent());
                body_open = ImGui::CollapsingHeader(group.label.c_str());
                ImGui::PopStyleColor();
            }
            if (!body_open) continue;

            for (const auto& item : group.items) {
                if (!matches_search(item, needle)) continue;
                ImGui::PushID(item.value.c_str());
                if (draw_row(item)) {
                    picked = item.value;
                    chose = true;
                }
                ImGui::PopID();
            }
        }

        if (!any_shown) {
            ImGui::TextDisabled("%s", config.loading ? config.loading_message
                                                     : config.empty_message);
        }
    }
    ImGui::EndChild();

    if (chose) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();

    if (chose) g_open_for.clear();
    return chose;
}

} // namespace fjell::grouped_picker
