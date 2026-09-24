#include "ui/kit/list_editor.hpp"

#include "ui/kit/button.hpp"
#include "ui/kit/edit_record.hpp"
#include "ui/kit/icons.hpp"
#include "ui/theme.hpp"

#include <imgui_internal.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace fjell::ui {

namespace {

// A card's insides: padding (left wider, where the grip is), the grip's and
// the number's widths.
constexpr float PAD_LEFT = 6.0f;
constexpr float PAD = 4.0f;
constexpr float GRIP_W = 16.0f;
constexpr float INDEX_W = 22.0f;
constexpr float INSERT_LINE = 2.0f;
constexpr float DASH = 4.0f;

// What a grip carries while an item is dragged: which list, which item.
struct Dragged {
    ImGuiID list;
    std::size_t index;
};
constexpr const char* PAYLOAD = "FJ_LIST_ITEM";

void dashed_rect(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 colour) {
    auto dashes = [&](ImVec2 a, ImVec2 b) {
        const float length = std::hypot(b.x - a.x, b.y - a.y);
        const ImVec2 dir{(b.x - a.x) / length, (b.y - a.y) / length};
        for (float t = 0.0f; t < length; t += DASH * 2.0f) {
            const float end = std::min(t + DASH, length);
            dl->AddLine({a.x + dir.x * t, a.y + dir.y * t}, {a.x + dir.x * end, a.y + dir.y * end},
                        colour);
        }
    };
    dashes(min, {max.x, min.y});
    dashes({max.x, min.y}, max);
    dashes(max, {min.x, max.y});
    dashes({min.x, max.y}, min);
}

} // namespace

ListEditor::ListEditor(const char* id, std::size_t count, ListOptions options)
    : options_{options} {
    ImGui::PushID(id);
    list_id_ = ImGui::GetID("##list");
    // A long list scrolls in a box of its own, so the rest of the panel
    // stays within reach.
    if (options_.max_cards > 0 && count > static_cast<std::size_t>(options_.max_cards)) {
        const float card = ImGui::GetFrameHeight() + PAD * 2.0f + theme::GAP_S;
        ImGui::BeginChild("##cards", {0.0f, card * static_cast<float>(options_.max_cards)});
        scrolling_ = true;
    }
}

ListEditor::~ListEditor() {
    end_scroll();
    ImGui::PopID();
}

void ListEditor::end_scroll() {
    if (!scrolling_) return;
    ImGui::EndChild();
    scrolling_ = false;
}

bool ListEditor::begin_item(std::size_t index, bool selected) {
    // A card's fields are not the row the list sits under.
    detail::set_edit_label(nullptr);
    index_ = index;
    selected_ = selected;
    clicked_ = false;
    ImGui::PushID(static_cast<int>(index));
    card_id_ = ImGui::GetID("##card");
    card_min_ = ImGui::GetCursorScreenPos();
    card_width_ = ImGui::GetContentRegionAvail().x;

    // The background goes behind the fields, whose height is only known
    // once they are drawn: it is drawn at the height the card had last frame.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const float row_h = ImGui::GetFrameHeight();
    const float height = storage->GetFloat(card_id_, row_h + PAD * 2.0f);
    const ImVec2 card_max{card_min_.x + card_width_, card_min_.y + height};

    // A card out of sight is passed over at the height it had.
    if (!ImGui::IsRectVisible(card_min_, card_max)) {
        ImGui::SetCursorScreenPos(
            {card_min_.x, card_max.y + theme::GAP_S - ImGui::GetStyle().ItemSpacing.y});
        ImGui::Dummy({card_width_, 0.0f});
        ImGui::PopID();
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(card_min_, card_max,
                      ImGui::GetColorU32(selected ? theme::selection() : theme::surface_sunken()),
                      ImGui::GetStyle().FrameRounding);

    // The card itself takes a click anywhere its fields don't: that selects
    // it. Everything drawn on it afterwards comes first.
    ImGui::SetNextItemAllowOverlap();
    clicked_ = ImGui::InvisibleButton("##select", {card_width_, height});

    // The grip: dragging it carries the item to another place in the list.
    ImGui::SetCursorScreenPos({card_min_.x + PAD_LEFT, card_min_.y + PAD});
    if (options_.reorder) {
        ImGui::InvisibleButton("##grip", {GRIP_W, row_h});
        const bool grip_hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (grip_hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            const Dragged dragged{list_id_, index};
            ImGui::SetDragDropPayload(PAYLOAD, &dragged, sizeof(dragged));
            ImGui::EndDragDropSource();
        }
        const float text_y = card_min_.y + PAD + ImGui::GetStyle().FramePadding.y;
        const float grip_icon_w = ImGui::CalcTextSize(icon::reorder).x;
        dl->AddText({card_min_.x + PAD_LEFT + (GRIP_W - grip_icon_w) * 0.5f, text_y},
                    ImGui::GetColorU32(grip_hot ? theme::text_secondary() : theme::text_disabled()),
                    icon::reorder);
    }

    // The item's number, counted from one, right-aligned; in the accent
    // on a selected card.
    char number[16];
    std::snprintf(number, sizeof(number), "%zu", index + 1);
    ImGui::PushFont(nullptr, theme::SMALL_TEXT);
    const ImVec2 number_size = ImGui::CalcTextSize(number);
    // A list that can't be reordered has no grip, and the number takes its place.
    const float grip_w = options_.reorder ? GRIP_W + theme::GAP_S : 0.0f;
    const float number_right = card_min_.x + PAD_LEFT + grip_w + INDEX_W;
    dl->AddText({number_right - number_size.x, card_min_.y + PAD + (row_h - number_size.y) * 0.5f},
                ImGui::GetColorU32(selected ? theme::accent() : theme::text_disabled()), number);
    ImGui::PopFont();

    // The item's own fields, between the number and the trash icon, on the
    // darker field colour a sunken card needs.
    const float content_x = number_right + theme::GAP_S + theme::GAP_XS;
    const float content_w = card_min_.x + card_width_ - PAD - row_h - theme::GAP_S - content_x;
    ImGui::SetCursorScreenPos({content_x, card_min_.y + PAD});
    ImGui::BeginGroup();
    // A first line of text (a card's summary or heading) sits on the field
    // line the number, grip and trash icon are centred on, not above it.
    ImGui::AlignTextToFramePadding();
    ImGui::PushItemWidth(std::max(content_w, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::surface_inset());
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme::surface_sunken());
    // Everything that sizes itself to the space left, a property table
    // included, ends at the card's content edge rather than under the trash
    // icon: the window's right edge is pulled in for the card's length.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    work_right_ = window->WorkRect.Max.x;
    content_right_ = window->ContentRegionRect.Max.x;
    const float right = content_x + std::max(content_w, 1.0f);
    window->WorkRect.Max.x = right;
    window->ContentRegionRect.Max.x = right;
    ImGui::PushClipRect({content_x, card_min_.y}, {right, FLT_MAX}, true);
    return true;
}

bool ListEditor::end_item() {
    ImGui::PopClipRect();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    window->WorkRect.Max.x = work_right_;
    window->ContentRegionRect.Max.x = content_right_;
    ImGui::PopStyleColor(2);
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    const float content_bottom = ImGui::GetItemRectMax().y;
    const float row_h = ImGui::GetFrameHeight();
    // At least one row tall, for the grip and the trash icon, padded both ends.
    const float height = std::max(content_bottom - card_min_.y, PAD + row_h) + PAD;
    ImGui::GetStateStorage()->SetFloat(card_id_, height);

    // The trash icon, always there: removing is undoable.
    bool remove = false;
    if (options_.remove) {
        ImGui::SetCursorScreenPos({card_min_.x + card_width_ - PAD - row_h, card_min_.y + PAD});
        remove = icon_button("##remove", icon::remove, "Remove", ButtonKind::GhostDanger);
    }

    // The whole card takes a dragged item of this list, landing it before
    // or after this one by which half the pointer is over.
    const ImRect card{card_min_, {card_min_.x + card_width_, card_min_.y + height}};
    if (options_.reorder && ImGui::BeginDragDropTargetCustom(card, card_id_)) {
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        if (payload != nullptr && payload->IsDataType(PAYLOAD)) {
            const auto* dragged = static_cast<const Dragged*>(payload->Data);
            if (dragged->list == list_id_) {
                const bool after = ImGui::GetMousePos().y > (card.Min.y + card.Max.y) * 0.5f;
                const std::size_t to = index_ + (after ? 1 : 0);
                const float y = after ? card.Max.y + theme::GAP_XS : card.Min.y - theme::GAP_XS;
                ImGui::GetWindowDrawList()->AddLine({card.Min.x, y}, {card.Max.x, y},
                                                    ImGui::GetColorU32(theme::accent()), INSERT_LINE);
                if (ImGui::AcceptDragDropPayload(PAYLOAD, ImGuiDragDropFlags_AcceptNoDrawDefaultRect)
                    && to != dragged->index && to != dragged->index + 1) {
                    moved_ = std::pair{dragged->index, to};
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Move below the card: an empty item placed so that the item spacing
    // after it leaves the gap between cards.
    ImGui::SetCursorScreenPos(
        {card_min_.x, card_min_.y + height + theme::GAP_S - ImGui::GetStyle().ItemSpacing.y});
    ImGui::Dummy({card_width_, 0.0f});
    ImGui::PopID();
    return remove;
}

void ListEditor::empty(const char* text) {
    end_scroll();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(nullptr, theme::SMALL_TEXT);
    const float wrap = width - theme::GAP_M * 2.0f;
    const ImVec2 size = ImGui::CalcTextSize(text, nullptr, false, wrap);
    const float height = size.y + theme::GAP_M * 2.0f + theme::GAP_XS;
    const ImVec2 max{min.x + width, min.y + height};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dashed_rect(dl, min, max, ImGui::GetColorU32(theme::surface_active()));
    ImGui::SetCursorScreenPos({min.x + std::max((width - size.x) * 0.5f, theme::GAP_M),
                               min.y + (height - size.y) * 0.5f});
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy({width, height});
}

bool ListEditor::add_button(const char* label) {
    end_scroll();
    const std::string text = std::string(icon::add) + "  " + label;
    return button(text.c_str(), ButtonKind::Ghost);
}

} // namespace fjell::ui
