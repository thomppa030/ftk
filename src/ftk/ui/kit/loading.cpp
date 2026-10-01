#include "ftk/ui/kit/loading.hpp"

#include "ftk/ui/theme.hpp"

#include <cmath>
#include <numbers>

namespace ftk::ui {

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
