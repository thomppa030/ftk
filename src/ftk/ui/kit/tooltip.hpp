#pragma once

// Tooltips (sheet 5): shown once the mouse rests, after the short delay,
// wrapped at theme::TOOLTIP_WRAP, a shortcut on the right in the secondary
// colour.
//
//     if (ui::button("Bake")) bake();
//     ui::item_tooltip("Bakes every probe in the scene", "Ctrl B");
//
//     if (dragging) ui::tooltip("Drop to parent it here");
//
//     if (auto tip = ui::ItemTooltip()) {  // a picture with its name
//         ui::image(thumbnail, {128.0f, 128.0f});
//         ui::text(path);
//     }

#include <string_view>

namespace ftk::ui {

/// The tooltip of the item just drawn, while the mouse rests on it.
void item_tooltip(std::string_view text, std::string_view shortcut = {});

/// A tooltip at the mouse this frame, whatever is under it: what a drag
/// would do, the value under a canvas's cursor. The last one asked for in a
/// frame is the one shown.
void tooltip(std::string_view text, std::string_view shortcut = {});

/// The tooltip of the item just drawn, holding what is drawn while it lives,
/// wrapped as the others are. True while it shows.
class ItemTooltip {
public:
    ItemTooltip();
    ~ItemTooltip();
    ItemTooltip(const ItemTooltip&) = delete;
    ItemTooltip& operator=(const ItemTooltip&) = delete;
    ItemTooltip(ItemTooltip&&) = delete;
    ItemTooltip& operator=(ItemTooltip&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

} // namespace ftk::ui
