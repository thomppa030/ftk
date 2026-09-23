#include "ui/kit/field.hpp"

#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>

namespace fjell::ui {

namespace {

constexpr float AXIS_LETTER_SIZE = 11.5f;
constexpr ImVec4 CLEAR{0.0f, 0.0f, 0.0f, 0.0f};

struct Axis {
    const char* letter;
    ImVec4 colour;
};

// ImGui draws a field's number itself, centred, in one colour. The kit
// draws it instead so the unit can follow in the secondary colour and an
// axis letter can lead: the field is drawn with its own text hidden, then
// the text goes on top. While the number is being typed ImGui's own text
// input is left alone.
bool hide_text(const char* id) {
    if (ImGui::TempInputIsActive(ImGui::GetID(id))) return false;
    ImGui::PushStyleColor(ImGuiCol_Text, CLEAR);
    return true;
}

// Whether a field's range lets its value go below zero. Such a field keeps
// room for the minus sign even while the value is positive, so the digits
// stay still when the value crosses zero.
bool can_be_negative(float min, float max) {
    return min >= max || min < 0.0f;
}

void draw_text(const char* number, Unit unit, const Axis* axis, bool signed_range) {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* symbol = unit_symbol(unit);

    const bool negative = number[0] == '-';
    const char* digits = negative ? number + 1 : number;
    const float sign_w = negative || signed_range ? ImGui::CalcTextSize("-").x : 0.0f;
    const float number_w = sign_w + ImGui::CalcTextSize(digits).x;
    const float unit_w = *symbol != '\0' ? theme::GAP_S + ImGui::CalcTextSize(symbol).x : 0.0f;
    const float left = min.x + style.FramePadding.x;
    const float y = std::floor((min.y + max.y - ImGui::GetFontSize()) * 0.5f);

    dl->PushClipRect(min, max, true);

    // The axis letter is pinned to the left edge, so the letters of a
    // vector line up down a column of rows.
    float text_left = left;
    if (axis != nullptr) {
        // Sit the smaller bold letter on the number's baseline.
        const float baseline = ImGui::GetFontBaked()->Ascent
            - theme::bold_font()->GetFontBaked(AXIS_LETTER_SIZE)->Ascent;
        dl->AddText(theme::bold_font(), AXIS_LETTER_SIZE, {left, y + std::floor(baseline)},
                    ImGui::GetColorU32(axis->colour), axis->letter);
        text_left += theme::bold_font()->CalcTextSizeA(AXIS_LETTER_SIZE, FLT_MAX, 0.0f,
                                                        axis->letter).x
                   + theme::GAP_S + theme::GAP_XS;
    }

    // The number and its unit are centred like ImGui's own text; one that
    // doesn't fit starts after the letter and is clipped on the right.
    float x = std::floor((min.x + max.x - number_w - unit_w) * 0.5f);
    if (x < text_left) x = text_left;
    if (negative) {
        dl->AddText({x, y}, ImGui::GetColorU32(ImGuiCol_Text), "-");
    }
    dl->AddText({x + sign_w, y}, ImGui::GetColorU32(ImGuiCol_Text), digits);
    if (*symbol != '\0') {
        dl->AddText({x + number_w + theme::GAP_S, y},
                    ImGui::GetColorU32(theme::text_secondary()), symbol);
    }
    dl->PopClipRect();
}

Edit finish(bool changed, bool hidden, const char* number, Unit unit, const Axis* axis,
            bool signed_range) {
    if (hidden) {
        ImGui::PopStyleColor();
        draw_text(number, unit, axis, signed_range);
    }
    return {changed, ImGui::IsItemDeactivatedAfterEdit()};
}

Edit drag_one(const char* id, float& value, const DragSpec& spec, const Axis* axis) {
    const bool hidden = hide_text(id);
    const bool changed = ImGui::DragFloat(id, &value, spec.speed, spec.min, spec.max, spec.format);
    char number[64];
    ImFormatString(number, sizeof(number), spec.format, value);
    return finish(changed, hidden, number, spec.unit, axis, can_be_negative(spec.min, spec.max));
}

Edit vec_n(const char* id, float* values, int count, const DragSpec& spec) {
    // A fourth component has no gizmo axis; its letter is plain.
    const Axis axes[] = {
        {"X", theme::axis_x()}, {"Y", theme::axis_y()}, {"Z", theme::axis_z()},
        {"W", theme::text_secondary()},
    };
    Edit edit;
    ImGui::BeginGroup();
    ImGui::PushID(id);
    ImGui::PushMultiItemsWidths(count, ImGui::CalcItemWidth());
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PushID(i);
        edit |= drag_one("##v", values[i], spec, &axes[i]);
        ImGui::PopID();
        ImGui::PopItemWidth();
    }
    ImGui::PopID();
    ImGui::EndGroup();
    return edit;
}

} // namespace

const char* unit_symbol(Unit unit) {
    switch (unit) {
        case Unit::None:            return "";
        case Unit::Metres:          return "m";
        case Unit::SquareMetres:    return "m\xc2\xb2";
        case Unit::MetresPerSecond: return "m/s";
        case Unit::Degrees:         return "\xc2\xb0";
        case Unit::Seconds:         return "s";
        case Unit::Minutes:         return "min";
        case Unit::Kilograms:       return "kg";
        case Unit::Pixels:          return "px";
        case Unit::Percent:         return "%";
        case Unit::Times:           return "\xc3\x97";
    }
    return "";
}

Edit drag(const char* id, float& value, const DragSpec& spec) {
    return drag_one(id, value, spec, nullptr);
}

static Edit drag_int_axis(const char* id, int& value, float speed, int min, int max, Unit unit,
                   const Axis* axis) {
    const bool hidden = hide_text(id);
    const bool changed = ImGui::DragInt(id, &value, speed, min, max);
    char number[32];
    ImFormatString(number, sizeof(number), "%d", value);
    return finish(changed, hidden, number, unit, axis,
                  can_be_negative(static_cast<float>(min), static_cast<float>(max)));
}

Edit drag_int(const char* id, int& value, float speed, int min, int max, Unit unit) {
    return drag_int_axis(id, value, speed, min, max, unit, nullptr);
}

Edit slider(const char* id, float& value, float min, float max, Unit unit, const char* format,
            bool logarithmic) {
    const bool hidden = hide_text(id);
    const bool changed = ImGui::SliderFloat(id, &value, min, max, format,
                                            logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
    char number[64];
    ImFormatString(number, sizeof(number), format, value);
    return finish(changed, hidden, number, unit, nullptr, can_be_negative(min, max));
}

Edit slider_int(const char* id, int& value, int min, int max, Unit unit) {
    const bool hidden = hide_text(id);
    const bool changed = ImGui::SliderInt(id, &value, min, max);
    char number[32];
    ImFormatString(number, sizeof(number), "%d", value);
    return finish(changed, hidden, number, unit, nullptr,
                  can_be_negative(static_cast<float>(min), static_cast<float>(max)));
}

Edit slider_labelled(const char* id, float& value, float min, float max, const char* text) {
    const bool hidden = hide_text(id);
    const bool changed = ImGui::SliderFloat(id, &value, min, max);
    return finish(changed, hidden, text, Unit::None, nullptr, false);
}

Edit ivec2(const char* id, glm::ivec2& value, float speed, int min, int max, Unit unit) {
    const Axis axes[] = {{"X", theme::axis_x()}, {"Y", theme::axis_y()}};
    Edit edit;
    ImGui::BeginGroup();
    ImGui::PushID(id);
    ImGui::PushMultiItemsWidths(2, ImGui::CalcItemWidth());
    for (int i = 0; i < 2; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PushID(i);
        edit |= drag_int_axis("##v", value[i], speed, min, max, unit, &axes[i]);
        ImGui::PopID();
        ImGui::PopItemWidth();
    }
    ImGui::PopID();
    ImGui::EndGroup();
    return edit;
}

Edit vec2(const char* id, glm::vec2& value, const DragSpec& spec) {
    return vec_n(id, &value.x, 2, spec);
}

Edit vec3(const char* id, glm::vec3& value, const DragSpec& spec) {
    return vec_n(id, &value.x, 3, spec);
}

Edit vec4(const char* id, glm::vec4& value, const DragSpec& spec) {
    return vec_n(id, &value.x, 4, spec);
}

void readout(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

Edit checkbox(const char* id, bool& value) {
    const bool changed = ImGui::Checkbox(id, &value);
    return {changed, changed};
}

} // namespace fjell::ui
