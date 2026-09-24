#include "ui/kit/viewport_toolbar.hpp"

#include "ui/theme.hpp"

namespace fjell::ui {

namespace {
constexpr float INSET = 8.0f;    // from the image's edge
constexpr float PAD = 3.0f;      // inside the pill
constexpr float SPACING = 2.0f;  // between its buttons
constexpr float ROUNDING = 6.0f;
constexpr float BACKING_ALPHA = 0.82f;
} // namespace

ViewportPill::ViewportPill(const char* id, ImVec2 image_min, ImVec2 image_max, PillPlace place) {
    ImGui::PushID(id);
    size_id_ = ImGui::GetID("##size");
    // Placed by the size its buttons took last frame, which only the right
    // and centre places need.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImVec2 size{storage->GetFloat(size_id_, 0.0f), ImGui::GetFrameHeight() + PAD * 2.0f};
    const float y = image_min.y + INSET;
    float x = image_min.x + INSET;
    if (place == PillPlace::TopRight) x = image_max.x - INSET - size.x;
    if (place == PillPlace::TopCenter) x = (image_min.x + image_max.x - size.x) * 0.5f;
    min_ = {x, y};

    // A dark backing, so its buttons read over a bright sky.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 backing = theme::surface_sunken();
    backing.w = BACKING_ALPHA;
    if (size.x > 0.0f) {
        dl->AddRectFilled(min_, {min_.x + size.x, min_.y + size.y}, ImGui::GetColorU32(backing), ROUNDING);
        dl->AddRect(min_, {min_.x + size.x, min_.y + size.y}, ImGui::GetColorU32(theme::border()), ROUNDING);
    }
    ImGui::SetCursorScreenPos({min_.x + PAD, min_.y + PAD});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {SPACING, 0.0f});
    ImGui::BeginGroup();
}

ViewportPill::~ViewportPill() {
    ImGui::EndGroup();
    ImGui::PopStyleVar();
    ImGui::GetStateStorage()->SetFloat(size_id_, ImGui::GetItemRectMax().x - min_.x + PAD);
    ImGui::PopID();
}

ViewportWindow::ViewportWindow(const char* title, bool* open, ImGuiWindowFlags flags) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.0f, 0.0f, 0.0f, 0.0f});
    visible_ = ImGui::Begin(title, open, flags);
}

ViewportWindow::~ViewportWindow() {
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace fjell::ui
