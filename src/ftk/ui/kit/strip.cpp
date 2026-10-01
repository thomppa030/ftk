#include "ftk/ui/kit/strip.hpp"

#include "ftk/ui/theme.hpp"

#include <algorithm>

namespace ftk::ui {

namespace {
constexpr float PAD_X = 12.0f;
constexpr float PAD_Y = 8.0f;
} // namespace

RaisedStrip::RaisedStrip(const char* id) {
    ImGui::PushID(id);
    height_id_ = ImGui::GetID("##height");
    min_ = ImGui::GetCursorScreenPos();
    width_ = ImGui::GetContentRegionAvail().x;
    // Drawn behind the contents at the height they had last frame.
    const float height = ImGui::GetStateStorage()->GetFloat(height_id_, ImGui::GetFrameHeight() + PAD_Y * 2.0f);
    ImGui::GetWindowDrawList()->AddRectFilled(min_, {min_.x + width_, min_.y + height},
                                              ImGui::GetColorU32(theme::surface_raised()),
                                              ImGui::GetStyle().FrameRounding + 2.0f);
    ImGui::SetCursorScreenPos({min_.x + PAD_X, min_.y + PAD_Y});
    ImGui::BeginGroup();
}

RaisedStrip::~RaisedStrip() {
    ImGui::EndGroup();
    const float height = std::max(ImGui::GetItemRectMax().y - min_.y + PAD_Y, ImGui::GetFrameHeight() + PAD_Y * 2.0f);
    ImGui::GetStateStorage()->SetFloat(height_id_, height);
    ImGui::SetCursorScreenPos({min_.x, min_.y + height});
    ImGui::Dummy({width_, 0.0f});
    ImGui::PopID();
}

} // namespace ftk::ui
