#include "ftk/ui/kit/section.hpp"

#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <cctype>
#include <optional>
#include <string>

namespace ftk::ui {

namespace {

constexpr float SECTION_SIZE = 13.0f;
constexpr float SUBHEADING_SIZE = 11.5f;
// The category dot before a group's name.
constexpr float DOT_DIAMETER = 7.0f;

std::string uppercase(const char* label) {
    std::string out(label);
    // The part after "##" only disambiguates the ID.
    if (auto hash = out.find("##"); hash != std::string::npos) out.resize(hash);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// One heading row: an optional rule above, then chevron, icon or category
// dot, and label in the secondary text colour. Folds when `foldable`;
// returns whether open.
bool heading(const char* label, const char* icon, float size, bool rule,
             bool foldable, bool default_open,
             std::optional<theme::Category> dot = std::nullopt) {
    // Under a search the matching rows stand on their own, every group open.
    if (detail::filtering()) return true;
    ImGui::PushID(label);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID open_id = ImGui::GetID("##open");
    bool open = !foldable || storage->GetBool(open_id, default_open);

    ImGui::Dummy({0.0f, theme::GAP_M});
    if (rule) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(
            p, {p.x + w, p.y}, ImGui::ColorConvertFloat4ToU32(theme::border()), 1.0f);
        ImGui::Dummy({0.0f, theme::GAP_S});
    }

    ImGui::PushFont(theme::bold_font(), size);
    if (foldable) {
        // The whole row is the click target, not just the chevron.
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::InvisibleButton("##fold", {w > 1.0f ? w : 1.0f, ImGui::GetTextLineHeight()})) {
            open = !open;
            storage->SetBool(open_id, open);
        }
        ImGui::SetCursorScreenPos(start);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    if (foldable) {
        ImGui::TextUnformatted(open ? icon::fold_open : icon::fold_closed);
        ImGui::SameLine(0.0f, theme::GAP_S);
    }
    if (icon != nullptr) {
        ImGui::TextUnformatted(icon);
        ImGui::SameLine(0.0f, theme::GAP_S + theme::GAP_XS);
    } else if (dot) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float r = DOT_DIAMETER * 0.5f;
        ImGui::GetWindowDrawList()->AddCircleFilled(
            {p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f}, r,
            ImGui::ColorConvertFloat4ToU32(theme::category(*dot)));
        ImGui::Dummy({DOT_DIAMETER, ImGui::GetTextLineHeight()});
        ImGui::SameLine(0.0f, theme::GAP_S);
    }
    const std::string text = uppercase(label);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // No spacer after: the label stays the last item, so a tooltip or a
    // test aims at the heading row, and item spacing separates what follows.
    ImGui::PopID();
    return open;
}

} // namespace

void section(const char* label, const char* icon) {
    heading(label, icon, SECTION_SIZE, true, false, true);
}

bool section_foldable(const char* label, const char* icon, bool default_open) {
    return heading(label, icon, SECTION_SIZE, true, true, default_open);
}

void subheading(const char* label) {
    heading(label, nullptr, SUBHEADING_SIZE, false, false, true);
}

bool subheading_foldable(const char* label, bool default_open) {
    return heading(label, nullptr, SUBHEADING_SIZE, false, true, default_open);
}

bool subheading_foldable(const char* label, theme::Category category, bool default_open) {
    return heading(label, nullptr, SUBHEADING_SIZE, false, true, default_open, category);
}

} // namespace ftk::ui
