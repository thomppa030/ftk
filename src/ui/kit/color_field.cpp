#include "ui/kit/color_field.hpp"

#include "core/math/color_space.hpp"
#include "ui/kit/text_field.hpp"
#include "ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace fjell::ui {

namespace {

int to_byte(float v) {
    return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

std::string to_hex(const float* rgba, bool alpha) {
    char hex[16];
    if (alpha) {
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X", to_byte(rgba[0]), to_byte(rgba[1]),
                      to_byte(rgba[2]), to_byte(rgba[3]));
    } else {
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", to_byte(rgba[0]), to_byte(rgba[1]),
                      to_byte(rgba[2]));
    }
    return hex;
}

// Reads "#RRGGBB" or "RRGGBB", plus "AA" when the colour has alpha. Leaves
// `rgba` alone and returns false for anything else.
bool from_hex(const std::string& text, float* rgba, bool alpha) {
    const std::size_t start = !text.empty() && text[0] == '#' ? 1 : 0;
    const std::size_t digits = text.size() - start;
    if (digits != 6 && !(alpha && digits == 8)) return false;
    float parsed[4]{0.0f, 0.0f, 0.0f, 1.0f};
    for (std::size_t i = 0; i < digits / 2; ++i) {
        unsigned byte = 0;
        const std::string pair = text.substr(start + i * 2, 2);
        if (pair.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
        if (std::sscanf(pair.c_str(), "%2x", &byte) != 1) return false;
        parsed[i] = static_cast<float>(byte) / 255.0f;
    }
    const std::size_t count = alpha ? 4 : 3;
    if (alpha && digits == 6) parsed[3] = rgba[3];
    std::copy(parsed, parsed + count, rgba);
    return true;
}

// The field over sRGB components, with or without alpha.
Edit srgb_field(const char* id, float* rgba, bool alpha) {
    Edit edit;
    const float side = ImGui::GetFrameHeight();
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float width = ImGui::CalcItemWidth();

    ImGui::PushID(id);
    ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoTooltip;
    if (!alpha) flags |= ImGuiColorEditFlags_NoAlpha;
    else flags |= ImGuiColorEditFlags_AlphaPreviewHalf;
    if (ImGui::ColorButton("##swatch", {rgba[0], rgba[1], rgba[2], alpha ? rgba[3] : 1.0f},
                           flags, {side, side})) {
        ImGui::OpenPopup("##picker");
    }

    ImGui::SameLine(0.0f, spacing);
    ImGui::SetNextItemWidth(std::max(width - side - spacing, 1.0f));
    std::string hex = to_hex(rgba, alpha);
    // The code in the smaller mono face, padded to the swatch's height.
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {padding.x, (side - theme::SMALL_TEXT) * 0.5f});
    ImGui::PushFont(theme::mono_font(), theme::SMALL_TEXT);
    if (text_field("##hex", hex).committed && from_hex(hex, rgba, alpha)) {
        edit = {true, true};
    }
    ImGui::PopFont();
    ImGui::PopStyleVar();

    if (ImGui::BeginPopup("##picker")) {
        ImGuiColorEditFlags picker = ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_DisplayHSV
                                   | ImGuiColorEditFlags_NoSidePreview;
        if (!alpha) picker |= ImGuiColorEditFlags_NoAlpha;
        else picker |= ImGuiColorEditFlags_AlphaBar;
        edit.changed = ImGui::ColorPicker4("##pick", rgba, picker) || edit.changed;
        edit.committed = ImGui::IsItemDeactivatedAfterEdit() || edit.committed;
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return edit;
}

} // namespace

Edit color(const char* id, glm::vec3& value, ColorSpace space) {
    const glm::vec3 shown = space == ColorSpace::Linear ? color_space::linear_to_srgb(value) : value;
    float rgba[4]{shown.r, shown.g, shown.b, 1.0f};
    const Edit edit = srgb_field(id, rgba, false);
    if (edit.changed) {
        const glm::vec3 srgb{rgba[0], rgba[1], rgba[2]};
        value = space == ColorSpace::Linear ? color_space::srgb_to_linear(srgb) : srgb;
    }
    return edit;
}

Edit color(const char* id, glm::vec4& value, ColorSpace space) {
    const glm::vec4 shown = space == ColorSpace::Linear ? color_space::linear_to_srgb(value) : value;
    float rgba[4]{shown.r, shown.g, shown.b, shown.a};
    const Edit edit = srgb_field(id, rgba, true);
    if (edit.changed) {
        const glm::vec4 srgb{rgba[0], rgba[1], rgba[2], rgba[3]};
        value = space == ColorSpace::Linear ? color_space::srgb_to_linear(srgb) : srgb;
    }
    return edit;
}

} // namespace fjell::ui
