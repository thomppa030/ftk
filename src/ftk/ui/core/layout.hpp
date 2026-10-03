#pragma once

// Where items go (decision 7 of the UI core plan). Each item takes its
// rectangle from a cursor when it is drawn, so it is hit-tested and drawn
// at once, in its real place. Items go down the area one under the other,
// unless the next is put on the same line.
//
// Measured containers lay out what needs the size of what comes after it.
// A container keeps its children's sizes from the last frame (the caller
// keeps the memory, under the container's id) and places this frame's by
// them: a spring shares out the room left, so what follows one spring sits
// at the far end and what lies between two is centred.
//
//     layout.begin_container({.axis = Axis::Row, .centre_across = true}, memory);
//     pill = layout.next({80, 20});
//     layout.spring();
//     button = layout.next({60, 24});  // at the right edge, by last frame's width
//     layout.end_container(kept);
//
// A wrapping row, a toolbar of tools, places each child as it comes and
// needs no memory. Points are in layout units.

#include "ftk/ui/core/rect.hpp"

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace ftk::ui::core {

struct LayoutStyle {
    /// Between items, across and down.
    glm::vec2 item_spacing{8.0f, 4.0f};
    /// The height new_line() leaves when nothing is on the line: a line of
    /// text.
    float empty_line{16.0f};
};

enum class Axis : uint8_t { Row, Column };

/// The most groups a container tells apart: springs past the seventh join
/// the last group.
inline constexpr std::size_t MAX_CONTAINER_GROUPS = 8;

/// What a container measured of its children, for placing them next frame.
struct ContainerMemory {
    /// Each group's length along the axis: the children between springs.
    std::array<float, MAX_CONTAINER_GROUPS> groups{};
    uint8_t group_count{0};
    /// The children's extent across the axis: the tallest in a row.
    float across{0.0f};

    bool operator==(const ContainerMemory&) const = default;
};

struct ContainerOptions {
    Axis axis{Axis::Row};
    /// Between children; below zero, the item spacing.
    float gap{-1.0f};
    /// Each child centred across the axis: in a row, in the row's height.
    bool centre_across{false};
    /// A row that goes onto another line when a child doesn't fit. It
    /// places children as they come, and needs no memory.
    bool wrap{false};
};

/// For widths: to the far edge of the room.
inline constexpr float FILL = -std::numeric_limits<float>::min();

class Layout {
public:
    void set_style(const LayoutStyle& style) { style_ = style; }
    [[nodiscard]] const LayoutStyle& style() const { return style_; }

    /// Forgets every scope, as at a root's start.
    void reset();

    /// Items go in `bounds` from its top-left, moved up and left by
    /// `scroll`, until end_area(), which returns the room they took from
    /// where they began.
    void begin_area(Rect bounds, glm::vec2 scroll = {0.0f, 0.0f});
    glm::vec2 end_area();

    /// The next item's rectangle, `size` big, at the cursor.
    Rect next(glm::vec2 size);
    /// The next item goes on the same line as the last, `gap` after it;
    /// below zero, the item spacing.
    void same_line(float gap = -1.0f);
    /// The next item goes on the line of the item before it: after it, as
    /// same_line() asked, or first in a group begun on its line.
    [[nodiscard]] bool continues_line() const;
    /// The last item placed went on the line of the one before it.
    [[nodiscard]] bool last_continued_line() const { return last_continued_line_; }
    /// Ends the line, or leaves an empty one when nothing is on it yet.
    void new_line();
    /// A little room under the last item; below zero, the item spacing.
    void spacing(float amount = -1.0f);
    /// Where the next item goes.
    [[nodiscard]] glm::vec2 cursor() const;
    /// Puts the next item at `at`.
    void set_cursor(glm::vec2 at);
    /// The room from the cursor to the edges of the area or the container.
    [[nodiscard]] glm::vec2 space_left() const;
    /// `width` against the room left: itself above zero, the room less as
    /// much below it, and all the room for FILL.
    [[nodiscard]] float resolve_width(float width) const;
    /// The last item placed, a group or a container counting as one.
    [[nodiscard]] Rect last_item() const { return last_item_; }
    /// The innermost area's bounds.
    [[nodiscard]] Rect area() const;

    /// What follows is one item to same_line() and last_item(), until
    /// end_group(), which returns its rectangle.
    void begin_group();
    Rect end_group();

    /// Children along `options.axis` in the room left, placed by what
    /// `memory` (null when there is none) measured of them, until
    /// end_container(). That returns the container's rectangle and writes
    /// what to keep for the next frame to `measured`.
    void begin_container(const ContainerOptions& options, const ContainerMemory* memory);
    /// Splits the children into groups: the room left over goes to the
    /// springs.
    void spring();
    Rect end_container(ContainerMemory& measured);

    [[nodiscard]] std::size_t depth() const { return scopes_.size(); }

private:
    enum class Kind : uint8_t { Area, Group, Container };

    struct Scope {
        Kind kind{Kind::Area};
        /// The room it may use.
        Rect bounds{};
        /// Where it began: its own position as an item.
        glm::vec2 start{0.0f};
        /// It began on the line of the item before it.
        bool continues_line{false};
        /// An area's: the last item before it, which it is again once the
        /// area ends (a region, placed just before its area; or what a
        /// panel was begun after).
        Rect item_before{};

        // Lines, in areas, groups and wrapping rows.
        float line_start_x{0.0f};
        glm::vec2 cursor{0.0f};
        float prev_end_x{0.0f};
        float prev_line_top{0.0f};
        float prev_line_height{0.0f};
        bool same_line{false};
        float same_line_gap{0.0f};

        bool has_items{false};
        Rect extent{};

        // Containers.
        ContainerOptions options{};
        const ContainerMemory* memory{nullptr};
        float gap{0.0f};
        float along{0.0f};
        float spring{0.0f};
        bool sprung{false};
        ContainerMemory measured{};
        bool group_has_items{false};
        float group_begin{0.0f};
        float group_end{0.0f};
    };

    struct Position {
        glm::vec2 at{0.0f};
        bool continues_line{false};
    };

    // Where an item `size` big goes in `scope`; without a size (a group or
    // a container beginning), where it starts.
    [[nodiscard]] Position position(const Scope& scope, const glm::vec2* size) const;
    // Takes `rect` as the next item of `scope`.
    void commit(Scope& scope, Rect rect, bool continues_line);
    // A container's position along its axis for group `group`, by memory.
    [[nodiscard]] float group_start(const Scope& scope, std::size_t group) const;
    void close_group(Scope& scope);

    [[nodiscard]] const Scope& top() const { return scopes_.back(); }
    Scope& top() { return scopes_.back(); }

    LayoutStyle style_{};
    std::vector<Scope> scopes_;
    Rect last_item_{};
    bool last_continued_line_{false};
};

} // namespace ftk::ui::core
