#pragma once

// Dragging something from where it is shown to where it should go: rows
// onto rows, an asset onto a field or a folder. A source says what it
// carries, a payload type such as "ASSET_PATH" and its bytes; a target
// takes what is of a type it knows. drag_preview() and draw_drop()
// (ui/kit/tree.hpp) draw what is dragged and where it would go. Esc ends a
// drag with nothing dropped: from then on no target is offered it and its
// source draws no preview, until the button is let go.
//
//     ui::tree_row({...});
//     if (auto drag = ui::DragSource()) {
//         drag.carry("ASSET_PATH", path);
//         ui::drag_preview(icon::animation, hue, name);
//     }
//
//     if (auto drop = ui::DropTarget()) {
//         if (auto payload = drop.dropped("ASSET_PATH")) open(payload->text());
//     }

#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <cstddef>
#include <optional>
#include <string_view>

namespace ftk::ui {

/// Files are dragged over the window from outside the program: a drop
/// target takes them as ui::OUTSIDE_FILES, their paths a line each once let
/// go.
[[nodiscard]] bool dragging_from_outside();

/// While it lives, the item just drawn is being dragged, when this is
/// true: carry() what it gives and draw the preview inside the scope. The
/// item may be one drawn by hand with no id of its own.
class DragSource {
public:
    DragSource();
    ~DragSource();
    DragSource(const DragSource&) = delete;
    DragSource& operator=(const DragSource&) = delete;
    DragSource(DragSource&&) = delete;
    DragSource& operator=(DragSource&&) = delete;

    explicit operator bool() const { return dragging_; }

    /// Gives the drag a copy of `size` bytes at `data`, of `type` (32
    /// characters at most).
    void carry(const char* type, const void* data, std::size_t size);

    /// Text, with the zero that ends it, so a target can read it either as
    /// Payload::text() or as a C string.
    void carry(const char* type, std::string_view text);

private:
    bool dragging_{false};
};

/// What is dropped, and whether the mouse has been let go.
struct Drop {
    Payload payload;
    /// The drop happens now; before it, the payload is only over the target.
    bool delivered{false};
};

/// While it lives, the item just drawn takes drops, when this is true:
/// while something is dragged over it. A payload's data is the UI's own
/// and good until the scope ends.
class DropTarget {
public:
    DropTarget();
    ~DropTarget();
    DropTarget(const DropTarget&) = delete;
    DropTarget& operator=(const DropTarget&) = delete;
    DropTarget(DropTarget&&) = delete;
    DropTarget& operator=(DropTarget&&) = delete;

    /// Over the whole of the window being drawn rather than an item: a
    /// panel that takes what is dropped anywhere on it.
    [[nodiscard]] static DropTarget window(const char* id);

    explicit operator bool() const { return open_; }

    /// What is dragged over it, when it is of `type`: for judging whether
    /// it may go here before taking it.
    [[nodiscard]] std::optional<Payload> offered(const char* type) const;

    /// What of `type` is dropped here, on the frame it is let go. The
    /// target is outlined while it is over it.
    [[nodiscard]] std::optional<Payload> dropped(const char* type);

    /// What of `type` is over it, every frame it is, with nothing drawn: the
    /// caller shows where it would go. `delivered` on the frame it is let go.
    [[nodiscard]] std::optional<Drop> take(const char* type);

private:
    DropTarget(const char* id, glm::vec2 min, glm::vec2 max);

    bool open_{false};
};

} // namespace ftk::ui
