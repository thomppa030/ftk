#pragma once

// A long run of rows one under another, of which only those in sight are
// drawn: the rows above and below them are passed over at once at the room
// they take, so what scrolls the run scrolls as though every row were
// there, and a run of a hundred thousand rows costs what a screenful does.
// Where each row sits is known before it is drawn: rows of one pitch (one
// of them perhaps of another), or rows whose tops are counted beforehand.
//
//     ui::RowsInView lines(entries.size(), {.pitch = line_pitch});
//     for (const std::size_t i : lines.rows()) {
//         lines.at(i);
//         draw_entry(entries[i]);
//     }
//
// The row that held the pointer or the keyboard on the last frame is drawn
// wherever it is, so a number dragged or a name being typed goes on while
// its row is scrolled away.

#include "ftk/ui/types.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace ftk::ui {

/// Where the rows of a run sit: each `pitch` below the one before (its
/// height and the gap under it), but for row `odd`, whose pitch is
/// `odd_pitch`: an open card among closed ones.
struct RowPitch {
    float pitch{0.0f};
    std::optional<std::size_t> odd{};
    float odd_pitch{0.0f};
};

class RowsInView {
public:
    /// `count` rows of a Tree, edge to edge at its row height. With
    /// `revealed`, the region scrolls to bring that row into view.
    explicit RowsInView(std::size_t count, std::optional<std::size_t> revealed = std::nullopt);
    /// `count` rows placed by `pitch`, the first at the cursor.
    RowsInView(std::size_t count, RowPitch pitch);
    /// Rows whose tops below the first row's are `tops`, the first 0, with
    /// one more at the end: where the last row's room ends. `tops` outlives
    /// the run.
    explicit RowsInView(std::span<const float> tops);
    /// Passes over the rows after the last drawn, if finish() hasn't.
    ~RowsInView();
    RowsInView(const RowsInView&) = delete;
    RowsInView& operator=(const RowsInView&) = delete;
    RowsInView(RowsInView&&) = delete;
    RowsInView& operator=(RowsInView&&) = delete;

    /// The rows to draw, in order: those in sight, and the one that held the
    /// pointer or the keyboard on the last frame, wherever it is.
    [[nodiscard]] const std::vector<std::size_t>& rows() const { return rows_; }
    /// Puts the cursor at the top of row `index`, one of rows(), past the
    /// rows not drawn since the last. Each row begins with it.
    void at(std::size_t index);
    /// Passes over the rows after the last drawn: what follows the run goes
    /// under it. For what is drawn after the run while it lives.
    void finish();
    /// Scrolls the region the run is in by as much as brings row `index`
    /// wholly into view, shown from the next frame on.
    void reveal(std::size_t index);

    /// The rows in sight, from first() to before end(), for a loop that
    /// draws them in order without at(); the row held on the last frame is
    /// not drawn then.
    [[nodiscard]] std::size_t first() const { return first_; }
    [[nodiscard]] std::size_t end() const { return end_; }

private:
    void start();
    // Row `index`'s top below the first row's; the count's is where the
    // last row's room ends.
    [[nodiscard]] float top_of(std::size_t index) const;
    // The row at `offset` below the first row's top; the gap under a row is
    // that row's. The count past the last.
    [[nodiscard]] std::size_t row_at(float offset) const;
    void pass_over(std::size_t index);
    // Ends the row being drawn, noting whether it took the pointer or the
    // keyboard.
    void close_row();

    std::size_t count_{0};
    RowPitch pitch_;
    std::span<const float> tops_;
    Id live_id_{0};
    float top_{0.0f};
    float width_{0.0f};
    std::size_t first_{0};
    std::size_t end_{0};
    std::vector<std::size_t> rows_;
    // The row whose top the cursor is at.
    std::size_t next_{0};
    bool placed_{false};
    std::optional<std::size_t> drawing_;
    Id live_before_{0};
    // The row drawn this frame that took the pointer or the keyboard.
    std::optional<std::size_t> live_;
    bool finished_{false};
};

} // namespace ftk::ui
