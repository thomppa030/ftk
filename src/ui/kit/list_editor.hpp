#pragma once

#include "ui/kit/edit.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <vector>

// A list of items the user adds to, removes from and reorders: animator
// clips, buoyancy points, spline points, terrain layers. Each item is a card
// with a drag grip, its number, its own fields and a trash icon; the Add
// button sits under the list, and an empty list says what it is for.
//
//     edit |= ui::list_editor("##points", buoyancy.points, "Add point",
//                             "No points. The collider's corners are used",
//                             [&](glm::vec3& p, size_t) { return ui::vec3("##p", p); });

namespace fjell::ui {

/// How a list behaves beyond the defaults.
struct ListOptions {
    /// Cards have a grip and can be dragged to a new place. Off for a list
    /// whose order is not the user's, such as spline points kept in key order.
    bool reorder{true};
    /// With more items than this, the cards scroll inside a box this many
    /// cards tall instead of lengthening the panel. 0 never scrolls.
    int max_cards{0};
};

/// Draws the cards one at a time; list_editor() below is the usual way in.
class ListEditor {
public:
    /// `count` is the number of items, to know whether they scroll.
    explicit ListEditor(const char* id, std::size_t count = 0, ListOptions options = {});
    ~ListEditor();
    ListEditor(const ListEditor&) = delete;
    ListEditor& operator=(const ListEditor&) = delete;
    ListEditor(ListEditor&&) = delete;
    ListEditor& operator=(ListEditor&&) = delete;

    /// Starts the card for item `index`; the item's fields follow, filling
    /// the card's width. A `selected` card is marked in the selection colour.
    /// Returns false for a card scrolled out of sight: it has been passed
    /// over, and neither its fields nor end_item() follow.
    bool begin_item(std::size_t index, bool selected = false);
    /// Ends the card. Returns true when its trash icon was clicked.
    bool end_item();
    /// Whether the card just ended was clicked outside its fields, which
    /// selects it or lets it go.
    [[nodiscard]] bool clicked() const { return clicked_; }

    /// The dashed box standing in for an empty list, saying what it holds.
    void empty(const char* text);
    /// The Add button under the list. Returns true when clicked.
    bool add_button(const char* label);

    /// An item dragged by its grip to a new place: from its index to the
    /// index it goes in front of (the count for the end).
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> moved() const { return moved_; }

private:
    void end_scroll();

    ImGuiID list_id_{0};
    ListOptions options_;
    bool scrolling_{false};
    std::size_t index_{0};
    bool selected_{false};
    bool clicked_{false};
    ImVec2 card_min_{};
    float card_width_{0.0f};
    ImGuiID card_id_{0};
    // The window's right edges while a card has them pulled in.
    float work_right_{0.0f};
    float content_right_{0.0f};
    std::optional<std::pair<std::size_t, std::size_t>> moved_;
};

/// Where the item at `index` is after the one at `from` moved in front of
/// `to`, for keeping a selection on the item it was on.
[[nodiscard]] inline std::size_t index_after_move(std::size_t index, std::size_t from, std::size_t to) {
    if (index == from) return to > from ? to - 1 : to;
    if (from < index && index < to) return index - 1;
    if (to <= index && index < from) return index + 1;
    return index;
}

/// A whole list: one card per item drawn by `draw_item(item, index)`, which
/// returns the item's Edit; adding appends `make()`. Adding, removing and
/// reordering are each a finished edit.
template <typename T, typename DrawItem, typename Make>
Edit list_editor(const char* id, std::vector<T>& items, const char* add_label,
                 const char* empty_text, DrawItem&& draw_item, Make&& make) {
    Edit edit;
    std::optional<std::size_t> remove;
    ListEditor list(id, items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!list.begin_item(i)) continue;
        edit |= draw_item(items[i], i);
        if (list.end_item()) remove = i;
    }
    if (items.empty()) list.empty(empty_text);
    if (list.add_button(add_label)) {
        items.push_back(make());
        edit = {true, true};
    }
    if (remove) {
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(*remove));
        edit = {true, true};
    } else if (auto move = list.moved()) {
        const auto [from, to] = *move;
        auto first = items.begin();
        if (from < to) {
            std::rotate(first + static_cast<std::ptrdiff_t>(from),
                        first + static_cast<std::ptrdiff_t>(from) + 1,
                        first + static_cast<std::ptrdiff_t>(to));
        } else {
            std::rotate(first + static_cast<std::ptrdiff_t>(to),
                        first + static_cast<std::ptrdiff_t>(from),
                        first + static_cast<std::ptrdiff_t>(from) + 1);
        }
        edit = {true, true};
    }
    return edit;
}

/// The same, adding a default-made item.
template <typename T, typename DrawItem>
Edit list_editor(const char* id, std::vector<T>& items, const char* add_label,
                 const char* empty_text, DrawItem&& draw_item) {
    return list_editor(id, items, add_label, empty_text, std::forward<DrawItem>(draw_item),
                       [] { return T{}; });
}

/// A list whose items are too long to show open at once (three rows or
/// more): each card shows `summary(item, index)` on one line, and only the
/// `selected` one (-1 for none) shows its fields through `draw_item`.
/// Clicking a card selects it or lets it go; selecting is not an edit.
template <typename T, typename Summary, typename DrawItem, typename Make>
Edit selectable_list_editor(const char* id, std::vector<T>& items, int& selected,
                            const char* add_label, const char* empty_text, ListOptions options,
                            Summary&& summary, DrawItem&& draw_item, Make&& make) {
    Edit edit;
    std::optional<std::size_t> remove;
    std::optional<int> select;
    {
        ListEditor list(id, items.size(), options);
        for (std::size_t i = 0; i < items.size(); ++i) {
            const bool open = static_cast<int>(i) == selected;
            if (!list.begin_item(i, open)) continue;
            if (open) {
                edit |= draw_item(items[i], i);
            } else {
                summary(items[i], i);
            }
            if (list.end_item()) remove = i;
            if (list.clicked()) select = open ? -1 : static_cast<int>(i);
        }
        if (items.empty()) list.empty(empty_text);
        if (list.add_button(add_label)) {
            items.push_back(make());
            selected = static_cast<int>(items.size()) - 1;
            edit = {true, true};
        }
        if (remove) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(*remove));
            if (selected == static_cast<int>(*remove)) selected = -1;
            else if (selected > static_cast<int>(*remove)) --selected;
            edit = {true, true};
        } else if (auto move = list.moved()) {
            const auto [from, to] = *move;
            auto first = items.begin();
            if (from < to) {
                std::rotate(first + static_cast<std::ptrdiff_t>(from),
                            first + static_cast<std::ptrdiff_t>(from) + 1,
                            first + static_cast<std::ptrdiff_t>(to));
            } else {
                std::rotate(first + static_cast<std::ptrdiff_t>(to),
                            first + static_cast<std::ptrdiff_t>(from),
                            first + static_cast<std::ptrdiff_t>(from) + 1);
            }
            if (selected >= 0) {
                selected = static_cast<int>(index_after_move(static_cast<std::size_t>(selected), from, to));
            }
            edit = {true, true};
        } else if (select) {
            selected = *select;
        }
    }
    return edit;
}

} // namespace fjell::ui
