#include "ui/kit/tree.hpp"

#include "ui/kit/feedback.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/search.hpp"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace fjell::ui {

namespace {

// Room at the row's ends, the chevron's column, the gap between the row's
// parts, the dots and the gap between them, and the corner.
constexpr float ROW_PAD = 4.0f;
constexpr float TWISTY = 14.0f;
constexpr float PART_GAP = 6.0f;
constexpr float DOT = 7.0f;
constexpr float DOT_GAP = 4.0f;
constexpr float DOTS_LEAD = 8.0f;
constexpr float ROUNDING = 3.0f;
// The insert line's thickness and the ring at its start.
constexpr float INSERT_LINE = 2.0f;
constexpr float RING = 8.0f;

} // namespace

void RenameBox::start(std::uint32_t key, std::string_view name) {
    key_ = key;
    original_ = std::string(name);
    text_ = original_;
    focus_ = true;
}

std::optional<std::string> RenameBox::draw(float width) {
    if (focus_) {
        ImGui::SetKeyboardFocusHere();
        focus_ = false;
    }
    // The key that started the edit (F2) turns on ImGui's keyboard-navigation
    // ring, which would draw around the field; the field is focus enough.
    ImGui::SetNavCursorVisible(false);
    ImGui::SetNextItemWidth(width);
    // Esc puts the text back as it was when the field was entered, so the
    // edit ends with the old name and changes nothing.
    ImGui::InputText("##rename", &text_, ImGuiInputTextFlags_AutoSelectAll);
    // A sunken field edged in amber: the name being edited.
    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                        ImGui::GetColorU32(theme::accent()), ImGui::GetStyle().FrameRounding);
    if (!ImGui::IsItemDeactivated()) return std::nullopt;
    key_.reset();
    if (text_.empty() || text_ == original_) return std::nullopt;
    return text_;
}

Tree::Tree(const char* id) {
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, 0.0f});
}

Tree::~Tree() {
    ImGui::PopStyleVar();
    ImGui::PopID();
}

TreeRowResult tree_row(const TreeRowSpec& spec) {
    TreeRowResult result;
    ImGui::PushID(spec.id);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID open_id = ImGui::GetID("##open");
    bool open = spec.has_children && storage->GetBool(open_id, true);

    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    const ImVec2 max{min.x + width, min.y + theme::TREE_ROW};
    const bool renaming = spec.rename != nullptr && spec.rename->editing(spec.key);

    // The whole row is the click target; the rename field sits over it.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##row", {width, theme::TREE_ROW});
    const bool hovered = ImGui::IsItemHovered();
    const float twisty_x = min.x + ROW_PAD + static_cast<float>(spec.depth) * theme::TREE_INDENT;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const float mouse_x = ImGui::GetIO().MousePos.x;
        if (spec.has_children && mouse_x >= twisty_x && mouse_x < twisty_x + TWISTY) {
            open = !open;
            storage->SetBool(open_id, open);
        } else {
            result.clicked = true;
        }
    }
    result.double_clicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (spec.selection == RowSelection::Primary) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::selection()), ROUNDING);
    } else if (spec.selection == RowSelection::Secondary) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::selection_secondary()), ROUNDING);
    } else if (hovered) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::surface_hover()), ROUNDING);
    }

    // A guide down through each level above, under its parent's chevron.
    const ImU32 guide = ImGui::GetColorU32(theme::surface_active());
    for (int level = 0; level < spec.depth; ++level) {
        const float x = std::floor(min.x + ROW_PAD + static_cast<float>(level) * theme::TREE_INDENT
                                   + TWISTY * 0.5f);
        dl->AddLine({x, min.y}, {x, max.y}, guide);
    }

    const float text_y = std::floor(min.y + (theme::TREE_ROW - ImGui::GetFontSize()) * 0.5f);
    if (spec.has_children) {
        const char* chevron = open || spec.force_open ? icon::fold_open : icon::fold_closed;
        const float w = ImGui::CalcTextSize(chevron).x;
        dl->AddText({twisty_x + (TWISTY - w) * 0.5f, text_y}, ImGui::GetColorU32(theme::text_disabled()),
                    chevron);
    }

    float x = twisty_x + TWISTY + PART_GAP;
    if (spec.icon != nullptr) {
        const ImVec4 tint = spec.category ? theme::category(*spec.category) : theme::text_secondary();
        dl->AddText({x, text_y}, ImGui::GetColorU32(tint), spec.icon);
        x += ImGui::CalcTextSize(spec.icon).x + PART_GAP;
    }

    // The dots at the right end, one per category.
    const float dots_w = spec.dots.empty()
        ? 0.0f
        : DOTS_LEAD + static_cast<float>(spec.dots.size()) * DOT
              + static_cast<float>(spec.dots.size() - 1) * DOT_GAP;
    float dot_x = max.x - ROW_PAD - dots_w + DOTS_LEAD;
    for (theme::Category category : spec.dots) {
        dl->AddCircleFilled({dot_x + DOT * 0.5f, (min.y + max.y) * 0.5f}, DOT * 0.5f,
                            ImGui::GetColorU32(theme::category(category)));
        dot_x += DOT + DOT_GAP;
    }

    const float name_right = max.x - ROW_PAD - dots_w;
    const char* name = spec.name.data();
    const char* name_end = name + spec.name.size();
    const float name_w = ImGui::CalcTextSize(name, name_end).x;
    if (renaming) {
        ImGui::SetCursorScreenPos({x - ImGui::GetStyle().FramePadding.x,
                                   min.y + (theme::TREE_ROW - ImGui::GetFrameHeight()) * 0.5f});
        result.renamed = spec.rename->draw(std::max(name_right - x, 1.0f));
    } else if (x + name_w <= name_right) {
        const ImU32 colour = ImGui::GetColorU32(spec.dimmed ? theme::text_disabled() : theme::text());
        detail::draw_highlighted(dl, {x, text_y}, spec.name, spec.dimmed ? std::string_view{} : spec.query,
                                 colour);
    } else {
        // Too long for the row: cut with an ellipsis, whole in the tooltip.
        ImGui::PushStyleColor(ImGuiCol_Text, spec.dimmed ? theme::text_disabled() : theme::text());
        ImGui::RenderTextEllipsis(dl, {x, text_y}, {name_right, max.y}, name_right, name, name_end, nullptr);
        ImGui::PopStyleColor();
        if (hovered) ImGui::SetItemTooltip("%.*s", static_cast<int>(spec.name.size()), name);
    }

    // Nothing but the rename field is an item after the row's button, so the
    // row stays the last item for the owner's menus and drags. The field
    // moved the cursor; the next row starts under this one.
    if (renaming) {
        ImGui::SetCursorScreenPos({min.x, max.y});
        ImGui::Dummy({0.0f, 0.0f});
    }
    ImGui::PopID();
    result.open = spec.has_children && (open || spec.force_open);
    return result;
}

DropPlace drop_place() {
    const float y = ImGui::GetIO().MousePos.y;
    const float top = ImGui::GetItemRectMin().y;
    const float height = ImGui::GetItemRectSize().y;
    if (y < top + height * 0.25f) return DropPlace::Before;
    if (y > top + height * 0.75f) return DropPlace::After;
    return DropPlace::Into;
}

void draw_drop(DropPlace place, int depth) {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 accent = ImGui::GetColorU32(theme::accent());
    if (place == DropPlace::Into) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::drop_fits()), ROUNDING);
        dl->AddRect(min, max, accent, ROUNDING);
        return;
    }
    // The line starts where a row at `depth` has its icon, so the ring's
    // indent says which parent the rows land under.
    const float y = std::floor(place == DropPlace::Before ? min.y : max.y);
    const float x = min.x + ROW_PAD + static_cast<float>(depth) * theme::TREE_INDENT + TWISTY + PART_GAP;
    dl->AddLine({x, y}, {max.x - ROW_PAD, y}, accent, INSERT_LINE);
    dl->AddCircleFilled({x, y}, RING * 0.5f, ImGui::GetColorU32(theme::surface_base()));
    dl->AddCircle({x, y}, RING * 0.5f - 1.0f, accent, 0, INSERT_LINE);
}

void drag_preview(const char* icon, std::optional<theme::Category> category, std::string_view text,
                  std::string_view refusal) {
    if (icon != nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, category ? theme::category(*category) : theme::text_secondary());
        ImGui::TextUnformatted(icon);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, PART_GAP);
    }
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    if (!refusal.empty()) {
        const std::string why(refusal);
        status(Severity::Error, why.c_str());
    }
}

} // namespace fjell::ui
