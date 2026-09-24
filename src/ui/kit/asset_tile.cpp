#include "ui/kit/asset_tile.hpp"

#include "ui/kit/search.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace fjell::ui {

namespace {

// The tile, its picture, the icon drawn when there is no picture, the room
// around them and between picture and name, and the corners.
constexpr float TILE_WIDTH = 96.0f;
constexpr float PICTURE = 80.0f;
constexpr float ICON = 40.0f;
constexpr float PAD_TOP = 8.0f;
constexpr float PAD_SIDE = 4.0f;
constexpr float PAD_BOTTOM = 6.0f;
constexpr float NAME_GAP = 5.0f;
constexpr float NAME_LINES = 2.0f;
constexpr float ROUNDING = 4.0f;

// Where each of the name's (at most two) lines ends, wrapping at `width`;
// `cut` is set when the name doesn't fit and the second line is to end in an
// ellipsis.
struct Lines {
    const char* end[2]{};
    int count{0};
    bool cut{false};
};

Lines wrap_name(const char* text, const char* text_end, float width) {
    Lines lines;
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const char* line = text;
    while (line < text_end && lines.count < 2) {
        const char* end = font->CalcWordWrapPosition(size, line, text_end, width);
        if (end == line) ++end;  // a word wider than the tile
        lines.end[lines.count++] = end;
        line = end;
        while (line < text_end && *line == ' ') ++line;
    }
    lines.cut = line < text_end;
    return lines;
}

// One line of the name, centred, with the part of the search's match that
// falls in it in amber. `match` and `match_end` are the whole name's match.
void draw_line(ImDrawList* dl, float left, float width, float y, const char* begin, const char* end,
               const char* match, const char* match_end, ImU32 colour) {
    const float w = ImGui::CalcTextSize(begin, end).x;
    float x = std::floor(left + std::max((width - w) * 0.5f, 0.0f));
    const ImU32 accent = ImGui::GetColorU32(theme::accent());
    const char* a = std::clamp(match, begin, end);
    const char* b = std::clamp(match_end, begin, end);
    for (auto [from, to, c] : {std::tuple{begin, a, colour}, std::tuple{a, b, accent}, std::tuple{b, end, colour}}) {
        if (from >= to) continue;
        dl->AddText({x, y}, c, from, to);
        x += ImGui::CalcTextSize(from, to).x;
    }
}

} // namespace

ImVec2 asset_tile_size() {
    const float name_h = ImGui::GetTextLineHeight() * NAME_LINES;
    return {TILE_WIDTH, PAD_TOP + PICTURE + NAME_GAP + name_h + PAD_BOTTOM};
}

AssetTileResult asset_tile(const AssetTileSpec& spec) {
    AssetTileResult result;
    ImGui::PushID(spec.id);
    const ImVec2 size = asset_tile_size();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max{min.x + size.x, min.y + size.y};
    const bool renaming = spec.rename != nullptr && spec.rename->editing(spec.key);

    // Only the rename field may sit over the tile. Allowing overlap
    // otherwise would let whatever the owner draws behind the grid (a
    // background taking right-clicks and drops) take the tile's mouse.
    if (renaming) ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##tile", size);
    const bool hovered = ImGui::IsItemHovered();
    result.clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    result.double_clicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    // Not while something is being dragged: ImGui draws the drag's preview
    // as a tooltip, and this one would replace it.
    if (hovered && !renaming && ImGui::GetDragDropPayload() == nullptr) {
        ImGui::SetItemTooltip("%.*s", static_cast<int>(spec.name.size()), spec.name.data());
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (spec.selected) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::selection()), ROUNDING);
    } else if (hovered) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::surface_hover()), ROUNDING);
    }

    // The picture, or the kind's icon, on a sunken square only while the
    // tile is hovered or selected: at rest the icon stands on the panel.
    const ImVec2 pic_min{std::floor(min.x + (size.x - PICTURE) * 0.5f), min.y + PAD_TOP};
    const ImVec2 pic_max{pic_min.x + PICTURE, pic_min.y + PICTURE};
    if (spec.picture != 0) {
        const ImU32 tint = spec.tint ? ImGui::GetColorU32(*spec.tint) : IM_COL32_WHITE;
        dl->AddImageRounded(spec.picture, pic_min, pic_max, {0.0f, 0.0f}, {1.0f, 1.0f}, tint, ROUNDING);
    } else {
        if (spec.selected || hovered) {
            dl->AddRectFilled(pic_min, pic_max, ImGui::GetColorU32(theme::surface_sunken()), ROUNDING);
        }
        if (spec.icon != nullptr) {
            const ImVec4 colour = spec.tint ? *spec.tint
                                : spec.category ? theme::category(*spec.category)
                                                : theme::text_secondary();
            ImFont* font = ImGui::GetFont();
            const ImVec2 icon = font->CalcTextSizeA(ICON, FLT_MAX, 0.0f, spec.icon);
            dl->AddText(font, ICON,
                        {std::floor(pic_min.x + (PICTURE - icon.x) * 0.5f), std::floor(pic_min.y + (PICTURE - icon.y) * 0.5f)},
                        ImGui::GetColorU32(colour), spec.icon);
        }
    }

    const float name_left = min.x + PAD_SIDE;
    const float name_width = size.x - PAD_SIDE * 2.0f;
    const float name_top = pic_max.y + NAME_GAP;
    if (renaming) {
        ImGui::SetCursorScreenPos({name_left, name_top});
        result.renamed = spec.rename->draw(name_width);
        ImGui::SetCursorScreenPos({min.x, max.y});
        ImGui::Dummy({0.0f, 0.0f});
    } else {
        const char* text = spec.name.data();
        const char* text_end = text + spec.name.size();
        const std::size_t at = detail::find_match(spec.name, spec.query);
        const char* match = spec.query.empty() || at == std::string_view::npos ? text_end : text + at;
        const char* match_end = match == text_end ? text_end : match + spec.query.size();
        const ImU32 colour = ImGui::GetColorU32(theme::text());
        const Lines lines = wrap_name(text, text_end, name_width);
        float y = name_top;
        const float line_h = ImGui::GetTextLineHeight();
        for (int i = 0; i < lines.count; ++i) {
            const char* begin = i == 0 ? text : lines.end[0];
            while (begin < text_end && *begin == ' ') ++begin;
            const bool last = i == lines.count - 1;
            if (last && lines.cut) {
                // What doesn't fit ends in an ellipsis; the tooltip has it all.
                ImGui::PushStyleColor(ImGuiCol_Text, theme::text());
                ImGui::RenderTextEllipsis(dl, {name_left, y}, {name_left + name_width, y + line_h},
                                          name_left + name_width, begin, text_end, nullptr);
                ImGui::PopStyleColor();
            } else {
                draw_line(dl, name_left, name_width, y, begin, lines.end[i], match, match_end, colour);
            }
            y += line_h;
        }
    }
    ImGui::PopID();
    return result;
}

} // namespace fjell::ui
