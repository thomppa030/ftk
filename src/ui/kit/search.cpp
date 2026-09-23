#include "ui/kit/search.hpp"

#include "ui/kit/icons.hpp"
#include "ui/theme.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>

namespace fjell::ui {

namespace detail {

std::size_t find_match(std::string_view text, std::string_view query) {
    if (query.empty()) return 0;
    auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    const auto found = std::ranges::search(text, query, [&](char a, char b) { return lower(a) == lower(b); });
    return found.empty() ? std::string_view::npos
                         : static_cast<std::size_t>(found.begin() - text.begin());
}

void draw_highlighted(ImDrawList* draw_list, ImVec2 pos, std::string_view text,
                      std::string_view query, ImU32 colour) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const std::size_t at = query.empty() ? std::string_view::npos : find_match(text, query);
    if (at == std::string_view::npos) {
        draw_list->AddText(pos, colour, begin, end);
        return;
    }
    const char* match = begin + at;
    const char* match_end = match + query.size();
    draw_list->AddText(pos, colour, begin, match);
    pos.x += ImGui::CalcTextSize(begin, match).x;
    draw_list->AddText(pos, ImGui::GetColorU32(theme::accent()), match, match_end);
    pos.x += ImGui::CalcTextSize(match, match_end).x;
    draw_list->AddText(pos, colour, match_end, end);
}

} // namespace detail

bool matches(std::string_view text, std::string_view query) {
    return query.empty() || detail::find_match(text, query) != std::string_view::npos;
}

void highlighted_text(std::string_view text, std::string_view query, bool dimmed) {
    const ImVec2 size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    const ImU32 colour = ImGui::GetColorU32(dimmed ? theme::text_disabled() : theme::text());
    // A dimmed parent has no match of its own to mark.
    detail::draw_highlighted(ImGui::GetWindowDrawList(), pos, text, dimmed ? std::string_view{} : query,
                             colour);
}

bool search_field(const char* id, std::string& query, const char* hint) {
    ImGui::PushID(id);
    const float width = ImGui::CalcItemWidth();
    const float height = ImGui::GetFrameHeight();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float icon_w = ImGui::CalcTextSize(icon::search).x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // Ctrl F reaches the field of the focused window; nothing else there
    // is using it.
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, ImGuiInputFlags_RouteFocused)) {
        ImGui::SetKeyboardFocusHere();
    }

    // The text starts after the icon; the clear button sits over the end.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        {style.FramePadding.x + icon_w + theme::GAP_S + theme::GAP_XS, style.FramePadding.y});
    ImGui::SetNextItemWidth(width);
    ImGui::SetNextItemAllowOverlap();
    bool changed = ImGui::InputTextWithHint("##query", hint, &query, ImGuiInputTextFlags_EscapeClearsAll);
    ImGui::PopStyleVar();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float text_y = origin.y + style.FramePadding.y;
    dl->AddText({origin.x + style.FramePadding.x, text_y}, ImGui::GetColorU32(theme::text_secondary()),
                icon::search);

    if (!query.empty()) {
        ImGui::SetCursorScreenPos({origin.x + width - height, origin.y});
        if (ImGui::InvisibleButton("##clear", {height, height})) {
            query.clear();
            changed = true;
        }
        ImGui::SetItemTooltip("Clear");
        const bool hovered = ImGui::IsItemHovered();
        const float clear_w = ImGui::CalcTextSize(icon::clear).x;
        dl->AddText({origin.x + width - height + (height - clear_w) * 0.5f, text_y},
                    ImGui::GetColorU32(hovered ? theme::text() : theme::text_secondary()), icon::clear);
        // Leave the layout under the field, not after the button.
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy({width, height});
    }
    ImGui::PopID();
    return changed;
}

} // namespace fjell::ui
