#pragma once

// Rows under named columns: a file list, the peers of a session. A row of
// a property and its value is ui::row's (ui/kit/row.hpp), not this.
//
//     static constexpr ui::Column COLUMNS[] = {{.name = "Peer"}, {.name = "RTT"}};
//     if (auto table = ui::Table("##peers", COLUMNS)) {
//         ui::table_headers();
//         for (const Peer& peer : peers) {
//             ui::table_next_row();
//             ui::table_next_column();
//             ui::text(peer.name);
//             ui::table_next_column();
//             ui::text(peer.rtt);
//         }
//     }

#include <cstdint>
#include <optional>
#include <span>

namespace ftk::ui {

enum class ColumnWidth : uint8_t {
    /// A share of what the fixed columns leave.
    Stretch,
    /// `width` pixels.
    Fixed,
};

struct Column {
    const char* name{""};
    ColumnWidth sizing{ColumnWidth::Stretch};
    float width{0.0f};
    /// The rows are sorted by this column at first.
    bool default_sort{false};
};

struct TableOptions {
    /// The columns' edges can be dragged.
    bool resizable{false};
    /// A click on a header sorts by its column; table_sort_changed() says how.
    bool sortable{false};
    /// The rows scroll under a header row that stays.
    bool scroll{false};
    /// Lines between the columns.
    bool column_lines{false};
    /// Stretched columns take shares in proportion to what is in them.
    bool proportional{false};
};

/// While it lives, rows go in the table `id` of `columns`. Draw them while
/// it is true.
class Table {
public:
    Table(const char* id, std::span<const Column> columns, TableOptions options = {});
    ~Table();
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    Table(Table&&) = delete;
    Table& operator=(Table&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

/// The row of the columns' names.
void table_headers();

/// Starts the next row.
void table_next_row();

/// Moves to the next cell of the row. False when its column is hidden.
bool table_next_column();

/// The mouse is on the row being drawn, in any of its cells: a property
/// row's menu opens from anywhere on it.
[[nodiscard]] bool table_row_hovered();

/// Which column the rows are to be sorted by, and which way.
struct TableSort {
    int column{0};
    bool ascending{true};
};

/// The sort, on the frame it changes (and on the first, for the default):
/// none otherwise. Asked after the headers.
[[nodiscard]] std::optional<TableSort> table_sort_changed();

} // namespace ftk::ui
