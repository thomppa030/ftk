#include "ftk/ui/kit/brand.hpp"

#include "ftk/ui/theme.hpp"

#include <cmath>
#include <initializer_list>
#include <numbers>

namespace ftk::ui {

namespace {

// The logo's drawing, in the coordinates of the SVG it was taken from: the
// box from (420, 48), 180 wide, the horizon at 185.
constexpr float LEFT = 420.0f;
constexpr float TOP = 48.0f;
constexpr float WIDTH = 180.0f;
constexpr float HEIGHT = 275.0f - TOP;   // down to the deepest reflection

ImU32 ridge(float alpha) {
    ImVec4 c = theme::text();
    c.w = alpha;
    return ImGui::GetColorU32(c);
}

} // namespace

ImVec2 logo_size(float scale) {
    return {WIDTH * scale, HEIGHT * scale};
}

void draw_logo(ImDrawList* dl, ImVec2 top_centre, float scale) {
    const float left = top_centre.x - WIDTH * 0.5f * scale;
    const auto pt = [&](float x, float y) {
        return ImVec2{left + (x - LEFT) * scale, top_centre.y + (y - TOP) * scale};
    };
    const auto line = [&](std::initializer_list<ImVec2> points, ImU32 colour, float width) {
        dl->AddPolyline(points.begin(), static_cast<int>(points.size()), colour, ImDrawFlags_None, width * scale);
    };

    // The ridges behind, then the peak in front in the accent.
    line({pt(420, 185), pt(445, 158), pt(465, 168), pt(485, 148), pt(510, 165), pt(535, 152), pt(555, 160),
          pt(575, 145), pt(600, 185)},
         ridge(0.22f), 1.3f);
    line({pt(420, 185), pt(450, 160), pt(475, 172), pt(495, 152), pt(515, 168), pt(540, 155), pt(565, 164),
          pt(585, 148), pt(600, 185)},
         ridge(0.45f), 1.8f);
    line({pt(420, 185), pt(510, 95), pt(600, 185)}, ImGui::GetColorU32(theme::accent()), 2.5f);
    // The horizon and the reflection under it.
    dl->AddLine(pt(418, 185), pt(602, 185), ridge(0.3f), 0.8f * scale);
    line({pt(420, 185), pt(510, 275), pt(600, 185)}, ridge(0.25f), 1.8f);
    line({pt(420, 185), pt(450, 210), pt(475, 198), pt(495, 218), pt(515, 202), pt(540, 215), pt(565, 206),
          pt(585, 222), pt(600, 185)},
         ridge(0.12f), 1.2f);
    line({pt(420, 185), pt(445, 212), pt(465, 202), pt(485, 222), pt(510, 205), pt(535, 218), pt(555, 210),
          pt(575, 225), pt(600, 185)},
         ridge(0.06f), 0.8f);
}

void wordmark(ImDrawList* dl, ImVec2 centre, float size) {
    constexpr const char* WORD = "FJELL";
    ImGui::PushFont(theme::bold_font(), size);
    const float spacing = size * 0.5f;
    float width = 0.0f;
    for (const char* c = WORD; *c != '\0'; ++c) width += ImGui::CalcTextSize(c, c + 1).x;
    width += spacing * 4.0f;
    float x = centre.x - width * 0.5f;
    const float y = centre.y - ImGui::GetFontSize() * 0.5f;
    for (const char* c = WORD; *c != '\0'; ++c) {
        dl->AddText({x, y}, ImGui::GetColorU32(theme::text()), c, c + 1);
        x += ImGui::CalcTextSize(c, c + 1).x + spacing;
    }
    ImGui::PopFont();
}

void spinner(ImDrawList* dl, ImVec2 centre, float radius, float seconds) {
    constexpr float TURN = 4.0f;   // radians a second
    const float start = seconds * TURN;
    dl->PathArcTo(centre, radius, start, start + std::numbers::pi_v<float> * 1.4f, 24);
    dl->PathStroke(ImGui::GetColorU32(theme::accent()), ImDrawFlags_None, 2.5f);
}

void loading(ImDrawList* dl, ImVec2 top_centre, std::string_view what, float seconds) {
    constexpr float RADIUS = 14.0f;
    spinner(dl, {top_centre.x, top_centre.y + RADIUS}, RADIUS, seconds);
    const char* begin = what.data();
    const char* end = what.data() + what.size();
    const float w = ImGui::CalcTextSize(begin, end).x;
    dl->AddText({std::floor(top_centre.x - w * 0.5f), top_centre.y + RADIUS * 2.0f + theme::GAP_L},
                ImGui::GetColorU32(theme::text_secondary()), begin, end);
}

} // namespace ftk::ui
