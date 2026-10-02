#pragma once

// A part of the window that scrolls on its own: a log, a file grid, the
// lanes of a track. It draws nothing of its own but a border when asked;
// a pane (ui/kit/pane.hpp) is the one with a surface.
//
//     if (auto tree = ui::Region("##tree", {220.0f, 0.0f}, {.border = true, .resize_x = true})) draw_tree();
//     ui::same_line();
//     if (auto grid = ui::Region("##grid")) draw_grid();

#include <glm/vec2.hpp>

namespace ftk::ui {

struct RegionOptions {
    bool border{false};
    /// Its right edge can be dragged to make it wider or narrower.
    bool resize_x{false};
    /// A scrollbar across its bottom when its lines are wider than it.
    bool scroll_x{false};
    /// The wheel scrolls it; off for a region that takes the wheel itself.
    bool wheel_scrolls{true};
};

/// While it lives, what is drawn goes in the region `id`, `size` big: zero
/// across or down takes all the room there is that way. Draw into it while
/// it is true; it is ended either way.
class Region {
public:
    explicit Region(const char* id, glm::vec2 size = {0.0f, 0.0f}, RegionOptions options = {});
    ~Region();
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;
    Region(Region&&) = delete;
    Region& operator=(Region&&) = delete;

    explicit operator bool() const { return visible_; }

private:
    bool visible_{false};
};

} // namespace ftk::ui
