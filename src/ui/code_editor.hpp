#pragma once

#include "ui/code_editor_syntax.hpp"

#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

namespace fjell {

class CodeEditor {
public:
    // Open a file (or switch to it if already open)
    void open(const std::string& file_path);

    // Close a specific tab
    void close_tab(int index);

    // Draw the full panel (tab bar + active editor)
    void draw(const char* title = "Code Editor");

    // Save the active file
    void save_active();

    [[nodiscard]] bool has_open_files() const { return !files_.empty(); }

    // Called after a file is saved. Args: path, language.
    std::function<void(const std::string&, Language)> on_save;

private:
    // --- Text buffer ---
    struct TextBuffer {
        std::vector<std::string> lines{""}; // always at least one line

        void set_text(const std::string& text);
        [[nodiscard]] std::string get_text() const;

        void insert_char(int line, int col, char c);
        void insert_text(int line, int col, const std::string& text);
        void insert_newline(int line, int col);
        void delete_char(int line, int col);              // delete char at pos
        void delete_char_before(int line, int& col_ref, int& line_ref); // backspace
        void delete_range(int l1, int c1, int l2, int c2);

        [[nodiscard]] int line_count() const { return static_cast<int>(lines.size()); }
        [[nodiscard]] int line_length(int line) const;
    };

    // --- Cursor ---
    struct Cursor {
        int line{0};
        int col{0};
        int sel_line{-1};  // -1 = no selection
        int sel_col{-1};
        float target_col{0.0f}; // sticky column for up/down

        [[nodiscard]] bool has_selection() const { return sel_line >= 0; }
        void clear_selection() { sel_line = -1; sel_col = -1; }
        void start_selection() {
            if (!has_selection()) { sel_line = line; sel_col = col; }
        }

        // Returns ordered selection (start before end)
        void selection_ordered(int& sl, int& sc, int& el, int& ec) const;
    };

    // --- Undo ---
    struct UndoEntry {
        std::string text;
        int cursor_line;
        int cursor_col;
    };

    // --- Open file ---
    struct OpenFile {
        std::string path;
        std::string display_name;
        TextBuffer buffer;
        Cursor cursor;
        Language language{Language::PlainText};
        float scroll_y{0.0f}; // in lines
        bool dirty{false};
        bool pending_focus{false};

        // Undo
        std::vector<UndoEntry> undo_stack;
        int undo_index{-1};
        float undo_timer{0.0f}; // seconds since last snapshot

        // Syntax cache
        std::vector<LineState> line_states;
        void rebuild_line_states();
        void invalidate_from(int line);
    };

    void handle_input(OpenFile& file);
    void render_editor(OpenFile& file);

    // Input helpers
    void move_cursor_left(OpenFile& f, bool select, bool word);
    void move_cursor_right(OpenFile& f, bool select, bool word);
    void move_cursor_up(OpenFile& f, bool select);
    void move_cursor_down(OpenFile& f, bool select);
    void move_cursor_home(OpenFile& f, bool select);
    void move_cursor_end(OpenFile& f, bool select);
    void handle_backspace(OpenFile& f, bool word);
    void handle_delete(OpenFile& f, bool word);
    void handle_enter(OpenFile& f);
    void handle_tab(OpenFile& f, bool shift);
    void handle_char(OpenFile& f, ImWchar c);
    void delete_selection(OpenFile& f);
    std::string get_selected_text(const OpenFile& f) const;
    void select_all(OpenFile& f);

    // Undo
    void push_undo(OpenFile& f);
    void undo(OpenFile& f);
    void redo(OpenFile& f);

    // Scroll
    void ensure_cursor_visible(OpenFile& f, float visible_lines);

    void update_font_metrics();

    std::vector<OpenFile> files_;
    int active_tab_{-1};
    ImFont* mono_font_{nullptr};
    float font_scale_{1.0f};
    float char_width_{0.0f};
    float line_height_{0.0f};
    float gutter_width_{0.0f};
};

} // namespace fjell
