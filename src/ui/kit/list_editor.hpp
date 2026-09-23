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

/// Draws the cards one at a time; list_editor() below is the usual way in.
class ListEditor {
public:
    explicit ListEditor(const char* id);
    ~ListEditor();
    ListEditor(const ListEditor&) = delete;
    ListEditor& operator=(const ListEditor&) = delete;
    ListEditor(ListEditor&&) = delete;
    ListEditor& operator=(ListEditor&&) = delete;

    /// Starts the card for item `index`; the item's fields follow, filling
    /// the card's width.
    void begin_item(std::size_t index);
    /// Ends the card. Returns true when its trash icon was clicked.
    bool end_item();

    /// The dashed box standing in for an empty list, saying what it holds.
    void empty(const char* text);
    /// The Add button under the list. Returns true when clicked.
    bool add_button(const char* label);

    /// An item dragged by its grip to a new place: from its index to the
    /// index it goes in front of (the count for the end).
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> moved() const { return moved_; }

private:
    ImGuiID list_id_{0};
    std::size_t index_{0};
    ImVec2 card_min_{};
    float card_width_{0.0f};
    ImGuiID card_id_{0};
    // The window's right edges while a card has them pulled in.
    float work_right_{0.0f};
    float content_right_{0.0f};
    std::optional<std::pair<std::size_t, std::size_t>> moved_;
};

/// A whole list: one card per item drawn by `draw_item(item, index)`, which
/// returns the item's Edit; adding appends `make()`. Adding, removing and
/// reordering are each a finished edit.
template <typename T, typename DrawItem, typename Make>
Edit list_editor(const char* id, std::vector<T>& items, const char* add_label,
                 const char* empty_text, DrawItem&& draw_item, Make&& make) {
    Edit edit;
    std::optional<std::size_t> remove;
    ListEditor list(id);
    for (std::size_t i = 0; i < items.size(); ++i) {
        list.begin_item(i);
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

} // namespace fjell::ui
