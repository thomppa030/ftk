#include "ftk/ui/kit/layout.hpp"

#include <imgui.h>

namespace ftk::ui {

void same_line() {
    ImGui::SameLine();
}

void same_line(float gap) {
    ImGui::SameLine(0.0f, gap);
}

void separator() {
    ImGui::Separator();
}

void spacing() {
    ImGui::Spacing();
}

void new_line() {
    ImGui::NewLine();
}

glm::vec2 cursor() {
    return ImGui::GetCursorScreenPos();
}

void set_cursor(glm::vec2 at) {
    ImGui::SetCursorScreenPos(at);
}

glm::vec2 space_left() {
    return ImGui::GetContentRegionAvail();
}

void next_width(float width) {
    ImGui::SetNextItemWidth(width);
}

float next_item_width() {
    return ImGui::CalcItemWidth();
}

void reserve(glm::vec2 size) {
    ImGui::Dummy(size);
}

void align_text_to_frame() {
    ImGui::AlignTextToFramePadding();
}

float frame_height() {
    return ImGui::GetFrameHeight();
}

float line_height() {
    return ImGui::GetTextLineHeight();
}

glm::vec2 text_size(std::string_view text) {
    return ImGui::CalcTextSize(text.data(), text.data() + text.size());
}

glm::vec2 item_spacing() {
    return ImGui::GetStyle().ItemSpacing;
}

glm::vec2 item_inner_spacing() {
    return ImGui::GetStyle().ItemInnerSpacing;
}

glm::vec2 frame_padding() {
    return ImGui::GetStyle().FramePadding;
}

Group::Group() {
    ImGui::BeginGroup();
}

Group::~Group() {
    ImGui::EndGroup();
}

} // namespace ftk::ui
