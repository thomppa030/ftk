#include "ftk/ui/kit/overlay.hpp"

#include "ftk/ui/theme.hpp"

#include <cmath>

namespace ftk::ui {

namespace {
constexpr float PLATE_ALPHA = 0.85f;
constexpr float PAD_X = 8.0f;
constexpr float PAD_Y = 3.0f;
constexpr float GAP = 6.0f;       // between the icon and the words, and between pieces
constexpr float KEY_PAD_X = 5.0f;
constexpr float KEY_PAD_Y = 1.0f;
constexpr float KEY_LIP = 2.0f;

float piece_width(const OverlayPiece& piece) {
    if (!piece.key) return ImGui::CalcTextSize(piece.text.data(), piece.text.data() + piece.text.size()).x;
    ImGui::PushFont(theme::bold_font(), 0.0f);
    const float w = ImGui::CalcTextSize(piece.text.data(), piece.text.data() + piece.text.size()).x;
    ImGui::PopFont();
    return w + KEY_PAD_X * 2.0f;
}

// A key as a small raised cap with a darker lip along its bottom, the name
// in the bold face (sheet 26, sized to a line of text).
void draw_key(ImDrawList* dl, ImVec2 pos, float width, std::string_view name) {
    const float h = ImGui::GetFontSize() + KEY_PAD_Y * 2.0f;
    const ImVec2 min{pos.x, pos.y - KEY_PAD_Y};
    const ImVec2 max{pos.x + width, min.y + h};
    const float rounding = ImGui::GetStyle().FrameRounding - 1.0f;
    dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::surface_raised()), rounding);
    dl->AddRectFilled({min.x, max.y - KEY_LIP}, max, ImGui::GetColorU32(theme::border()), rounding,
                      ImDrawFlags_RoundCornersBottom);
    ImGui::PushFont(theme::bold_font(), 0.0f);
    dl->AddText({pos.x + KEY_PAD_X, pos.y}, ImGui::GetColorU32(theme::text()), name.data(),
                name.data() + name.size());
    ImGui::PopFont();
}
} // namespace

void overlay_plate(ImDrawList* draw_list, ImVec2 min, ImVec2 max) {
    ImVec4 plate = theme::surface_sunken();
    plate.w = PLATE_ALPHA;
    draw_list->AddRectFilled(min, max, ImGui::GetColorU32(plate), ImGui::GetStyle().FrameRounding);
}

ImVec2 corner_note(ImDrawList* draw_list, ImVec2 at, Corner corner, const CornerNoteSpec& spec) {
    // Keys stand a little taller than the words, so a line holding one gets
    // their height.
    bool has_key = false;
    float line_w = 0.0f;
    if (spec.icon != nullptr) line_w += ImGui::CalcTextSize(spec.icon).x + GAP;
    for (std::size_t i = 0; i < spec.line.size(); ++i) {
        line_w += piece_width(spec.line[i]) + (i > 0 ? GAP : 0.0f);
        has_key |= spec.line[i].key;
    }
    const float pad_y = has_key ? PAD_Y + KEY_PAD_Y : PAD_Y;
    const ImVec2 size{std::floor(line_w + PAD_X * 2.0f), ImGui::GetFontSize() + pad_y * 2.0f};

    const bool right = corner == Corner::TopRight || corner == Corner::BottomRight;
    const bool bottom = corner == Corner::BottomLeft || corner == Corner::BottomRight;
    const ImVec2 min{std::floor(right ? at.x - size.x : at.x), std::floor(bottom ? at.y - size.y : at.y)};
    overlay_plate(draw_list, min, {min.x + size.x, min.y + size.y});

    const ImU32 ink = ImGui::GetColorU32(spec.ink);
    float x = min.x + PAD_X;
    const float y = min.y + pad_y;
    if (spec.icon != nullptr) {
        draw_list->AddText({x, y}, spec.icon_ink ? ImGui::GetColorU32(*spec.icon_ink) : ink, spec.icon);
        x += ImGui::CalcTextSize(spec.icon).x + GAP;
    }
    for (const OverlayPiece& piece : spec.line) {
        const float w = piece_width(piece);
        if (piece.key) {
            draw_key(draw_list, {x, y}, w, piece.text);
        } else {
            draw_list->AddText({x, y}, ink, piece.text.data(), piece.text.data() + piece.text.size());
        }
        x += w + GAP;
    }
    return size;
}

} // namespace ftk::ui
