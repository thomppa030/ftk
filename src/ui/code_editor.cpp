#include "ui/code_editor.hpp"
#include "ui/theme.hpp"
#include "core/log.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fjell {

// sRGB 0–255 → linear ImU32 (Vulkan sRGB framebuffer needs linear values)
static ImU32 srgb_u32(int r, int g, int b, int a = 255) {
    auto to_lin = [](int v) -> int {
        float s = static_cast<float>(v) / 255.0f;
        return static_cast<int>(theme::srgb_to_linear(s) * 255.0f + 0.5f);
    };
    return IM_COL32(to_lin(r), to_lin(g), to_lin(b), a);
}

// ===========================================================================
// TextBuffer
// ===========================================================================

void CodeEditor::TextBuffer::set_text(const std::string& text) {
    lines.clear();
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        // Strip trailing \r for Windows line endings
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    if (lines.empty()) {
        lines.emplace_back();
    }
    // If text ends with newline, add empty line
    if (!text.empty() && text.back() == '\n') {
        lines.emplace_back();
    }
}

std::string CodeEditor::TextBuffer::get_text() const {
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        result += lines[i];
        if (i + 1 < lines.size()) {
            result += '\n';
        }
    }
    return result;
}

void CodeEditor::TextBuffer::insert_char(int line, int col, char c) {
    if (line < 0 || line >= line_count()) return;
    auto& l = lines[line];
    col = std::clamp(col, 0, static_cast<int>(l.size()));
    l.insert(l.begin() + col, c);
}

void CodeEditor::TextBuffer::insert_text(int line, int col, const std::string& text) {
    if (line < 0 || line >= line_count()) return;
    auto& l = lines[line];
    col = std::clamp(col, 0, static_cast<int>(l.size()));

    // Handle multi-line insertion
    std::istringstream stream(text);
    std::string first_part;
    if (std::getline(stream, first_part)) {
        std::string after = l.substr(col);
        l = l.substr(0, col) + first_part;

        std::string next_line;
        int insert_at = line;
        while (std::getline(stream, next_line)) {
            ++insert_at;
            lines.insert(lines.begin() + insert_at, next_line);
        }
        // Append remainder to last inserted line
        lines[insert_at] += after;
    }
}

void CodeEditor::TextBuffer::insert_newline(int line, int col) {
    if (line < 0 || line >= line_count()) return;
    auto& l = lines[line];
    col = std::clamp(col, 0, static_cast<int>(l.size()));
    std::string after = l.substr(col);
    l.erase(col);

    // Auto-indent: copy leading whitespace from current line
    std::string indent;
    for (char c : lines[line]) {
        if (c == ' ' || c == '\t') {
            indent += c;
        } else {
            break;
        }
    }

    lines.insert(lines.begin() + line + 1, indent + after);
}

void CodeEditor::TextBuffer::delete_char(int line, int col) {
    if (line < 0 || line >= line_count()) return;
    auto& l = lines[line];
    if (col < static_cast<int>(l.size())) {
        l.erase(col, 1);
    } else if (line + 1 < line_count()) {
        // Join with next line
        l += lines[line + 1];
        lines.erase(lines.begin() + line + 1);
    }
}

void CodeEditor::TextBuffer::delete_char_before(int line, int& col_ref, int& line_ref) {
    if (col_ref > 0) {
        lines[line].erase(col_ref - 1, 1);
        --col_ref;
    } else if (line > 0) {
        // Join with previous line
        int prev_len = static_cast<int>(lines[line - 1].size());
        lines[line - 1] += lines[line];
        lines.erase(lines.begin() + line);
        line_ref = line - 1;
        col_ref = prev_len;
    }
}

void CodeEditor::TextBuffer::delete_range(int l1, int c1, int l2, int c2) {
    if (l1 > l2 || (l1 == l2 && c1 > c2)) {
        std::swap(l1, l2);
        std::swap(c1, c2);
    }
    if (l1 < 0) l1 = 0;
    if (l2 >= line_count()) l2 = line_count() - 1;

    if (l1 == l2) {
        auto& ln = lines[l1];
        c1 = std::clamp(c1, 0, static_cast<int>(ln.size()));
        c2 = std::clamp(c2, 0, static_cast<int>(ln.size()));
        ln.erase(c1, c2 - c1);
    } else {
        auto& first = lines[l1];
        auto& last = lines[l2];
        c1 = std::clamp(c1, 0, static_cast<int>(first.size()));
        c2 = std::clamp(c2, 0, static_cast<int>(last.size()));
        std::string merged = first.substr(0, c1) + last.substr(c2);
        lines[l1] = std::move(merged);
        lines.erase(lines.begin() + l1 + 1, lines.begin() + l2 + 1);
    }
}

int CodeEditor::TextBuffer::line_length(int line) const {
    if (line < 0 || line >= line_count()) return 0;
    return static_cast<int>(lines[line].size());
}

// ===========================================================================
// Cursor
// ===========================================================================

void CodeEditor::Cursor::selection_ordered(int& sl, int& sc, int& el, int& ec) const {
    if (!has_selection()) {
        sl = sc = el = ec = -1;
        return;
    }
    if (sel_line < line || (sel_line == line && sel_col < col)) {
        sl = sel_line; sc = sel_col;
        el = line; ec = col;
    } else {
        sl = line; sc = col;
        el = sel_line; ec = sel_col;
    }
}

// ===========================================================================
// Syntax cache
// ===========================================================================

void CodeEditor::OpenFile::rebuild_line_states() {
    line_states.resize(buffer.line_count());
    LineState state{};
    for (int i = 0; i < buffer.line_count(); ++i) {
        std::vector<Token> tokens;
        state = tokenize_line(buffer.lines[i], language, state, tokens);
        line_states[i] = state;
    }
}

void CodeEditor::OpenFile::invalidate_from(int line) {
    if (line < 0) line = 0;
    if (line_states.empty()) {
        rebuild_line_states();
        return;
    }
    line_states.resize(buffer.line_count());
    LineState state = (line > 0) ? line_states[line - 1] : LineState{};
    for (int i = line; i < buffer.line_count(); ++i) {
        std::vector<Token> tokens;
        LineState new_state = tokenize_line(buffer.lines[i], language, state, tokens);
        bool same = (i < static_cast<int>(line_states.size()) &&
                     line_states[i].in_block_comment == new_state.in_block_comment);
        line_states[i] = new_state;
        state = new_state;
        // If state stabilized, we can stop
        if (same && i > line) break;
    }
}

// ===========================================================================
// CodeEditor — file management
// ===========================================================================

void CodeEditor::open(const std::string& file_path) {
    // Check if already open
    for (int i = 0; i < static_cast<int>(files_.size()); ++i) {
        if (files_[i].path == file_path) {
            active_tab_ = i;
            return;
        }
    }

    // Read file
    std::ifstream in(file_path);
    if (!in.is_open()) {
        FJELL_CORE_ERROR("CodeEditor: failed to open {}", file_path);
        return;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());

    OpenFile file;
    file.path = file_path;
    file.display_name = std::filesystem::path(file_path).filename().string();
    file.language = detect_language(file_path);
    file.buffer.set_text(content);
    file.pending_focus = true;
    file.rebuild_line_states();

    // Initial undo snapshot
    file.undo_stack.push_back({content, 0, 0});
    file.undo_index = 0;

    files_.push_back(std::move(file));
    active_tab_ = static_cast<int>(files_.size()) - 1;

    FJELL_CORE_INFO("CodeEditor: opened {}", file_path);
}

void CodeEditor::close_tab(int index) {
    if (index < 0 || index >= static_cast<int>(files_.size())) return;
    files_.erase(files_.begin() + index);
    if (active_tab_ >= static_cast<int>(files_.size())) {
        active_tab_ = static_cast<int>(files_.size()) - 1;
    }
}

void CodeEditor::save_active() {
    if (active_tab_ < 0 || active_tab_ >= static_cast<int>(files_.size())) return;
    auto& file = files_[active_tab_];

    {
        std::ofstream out(file.path);
        if (!out.is_open()) {
            FJELL_CORE_ERROR("CodeEditor: failed to save {}", file.path);
            return;
        }
        std::string text = file.buffer.get_text();
        out << text;
        if (!text.empty() && text.back() != '\n') {
            out << '\n';
        }
    } // flush + close before triggering reload

    file.dirty = false;
    FJELL_CORE_INFO("CodeEditor: saved {}", file.path);

    if (on_save) {
        on_save(file.path, file.language);
    }
}

// ===========================================================================
// Draw
// ===========================================================================

void CodeEditor::update_font_metrics() {
    float scaled_size = mono_font_->LegacySize * font_scale_;
    char_width_ = mono_font_->CalcTextSizeA(scaled_size, FLT_MAX, 0.0f, "M").x;
    line_height_ = scaled_size + 1.0f;
}

void CodeEditor::draw(const char* title) {
    // Grab mono font on first use
    auto& io = ImGui::GetIO();
    if (!mono_font_ && io.Fonts->Fonts.Size > 2) {
        mono_font_ = io.Fonts->Fonts[2];
        update_font_metrics();
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    if (ImGui::Begin(title)) {
        if (files_.empty()) {
            ImGui::SetCursorPos({12.0f, 12.0f});
            ImGui::TextDisabled("Open a file from the Assets panel");
        } else {
            // Tab bar
            if (ImGui::BeginTabBar("##code_tabs", ImGuiTabBarFlags_Reorderable |
                                                   ImGuiTabBarFlags_AutoSelectNewTabs)) {
                for (int i = 0; i < static_cast<int>(files_.size()); ++i) {
                    auto& file = files_[i];
                    ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
                    if (file.dirty) {
                        flags |= ImGuiTabItemFlags_UnsavedDocument;
                    }
                    if (file.pending_focus) {
                        flags |= ImGuiTabItemFlags_SetSelected;
                        file.pending_focus = false;
                    }

                    bool open = true;
                    ImGui::PushID(i);
                    if (ImGui::BeginTabItem(file.display_name.c_str(), &open, flags)) {
                        active_tab_ = i;
                        ImGui::EndTabItem();
                    }
                    ImGui::PopID();

                    if (!open) {
                        close_tab(i);
                        --i; // adjust for erased element
                    }
                }
                ImGui::EndTabBar();
            }

            // Active editor
            if (active_tab_ >= 0 && active_tab_ < static_cast<int>(files_.size())) {
                auto& file = files_[active_tab_];
                if (mono_font_) {
                    ImGui::PushFont(mono_font_);
                    handle_input(file);
                    render_editor(file);
                    ImGui::PopFont();
                }
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// ===========================================================================
// Input handling
// ===========================================================================

static bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

void CodeEditor::handle_input(OpenFile& f) {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;

    auto& io = ImGui::GetIO();
    bool ctrl = io.KeyCtrl;
    bool shift = io.KeyShift;

    // Ctrl+S is handled globally in Engine — don't duplicate here

    // Undo/redo
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        if (shift) {
            redo(f);
        } else {
            undo(f);
        }
        return;
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
        redo(f);
        return;
    }

    // Select all
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
        select_all(f);
        return;
    }

    // Copy
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
        if (f.cursor.has_selection()) {
            ImGui::SetClipboardText(get_selected_text(f).c_str());
        }
        return;
    }

    // Cut
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X)) {
        if (f.cursor.has_selection()) {
            ImGui::SetClipboardText(get_selected_text(f).c_str());
            push_undo(f);
            delete_selection(f);
            f.dirty = true;
            f.invalidate_from(f.cursor.line);
        }
        return;
    }

    // Paste
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
        const char* clip = ImGui::GetClipboardText();
        if (clip && clip[0] != '\0') {
            push_undo(f);
            if (f.cursor.has_selection()) {
                delete_selection(f);
            }
            std::string text(clip);
            // Count lines in pasted text to position cursor
            int start_line = f.cursor.line;
            f.buffer.insert_text(f.cursor.line, f.cursor.col, text);
            // Move cursor to end of pasted text
            int newlines = 0;
            int last_line_len = 0;
            for (char c : text) {
                if (c == '\n') {
                    ++newlines;
                    last_line_len = 0;
                } else {
                    ++last_line_len;
                }
            }
            if (newlines > 0) {
                f.cursor.line = start_line + newlines;
                f.cursor.col = last_line_len;
            } else {
                f.cursor.col += static_cast<int>(text.size());
            }
            f.cursor.clear_selection();
            f.dirty = true;
            f.invalidate_from(start_line);
        }
        return;
    }

    // Navigation
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  { move_cursor_left(f, shift, ctrl); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { move_cursor_right(f, shift, ctrl); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    { move_cursor_up(f, shift); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))   { move_cursor_down(f, shift); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        if (ctrl) {
            if (shift) f.cursor.start_selection();
            f.cursor.line = 0;
            f.cursor.col = 0;
            if (!shift) f.cursor.clear_selection();
        } else {
            move_cursor_home(f, shift);
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End)) {
        if (ctrl) {
            if (shift) f.cursor.start_selection();
            f.cursor.line = f.buffer.line_count() - 1;
            f.cursor.col = f.buffer.line_length(f.cursor.line);
            if (!shift) f.cursor.clear_selection();
        } else {
            move_cursor_end(f, shift);
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
        if (shift) f.cursor.start_selection();
        f.cursor.line = std::max(0, f.cursor.line - 20);
        f.cursor.col = std::min(f.cursor.col, f.buffer.line_length(f.cursor.line));
        if (!shift) f.cursor.clear_selection();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
        if (shift) f.cursor.start_selection();
        f.cursor.line = std::min(f.buffer.line_count() - 1, f.cursor.line + 20);
        f.cursor.col = std::min(f.cursor.col, f.buffer.line_length(f.cursor.line));
        if (!shift) f.cursor.clear_selection();
        return;
    }

    // Editing
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) { handle_backspace(f, ctrl); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete))    { handle_delete(f, ctrl); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        handle_enter(f);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) { handle_tab(f, shift); return; }

    // Character input
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        ImWchar c = io.InputQueueCharacters[i];
        if (c >= 32 && c != 127) {
            handle_char(f, c);
        }
    }
    io.InputQueueCharacters.resize(0);
}

// ===========================================================================
// Cursor movement
// ===========================================================================

void CodeEditor::move_cursor_left(OpenFile& f, bool select, bool word) {
    if (select) f.cursor.start_selection();

    if (word) {
        // Skip whitespace, then skip word chars (or vice versa)
        if (f.cursor.col > 0) {
            auto& line = f.buffer.lines[f.cursor.line];
            int c = f.cursor.col;
            while (c > 0 && !is_word_char(line[c - 1])) --c;
            while (c > 0 && is_word_char(line[c - 1])) --c;
            f.cursor.col = c;
        } else if (f.cursor.line > 0) {
            --f.cursor.line;
            f.cursor.col = f.buffer.line_length(f.cursor.line);
        }
    } else {
        if (f.cursor.col > 0) {
            --f.cursor.col;
        } else if (f.cursor.line > 0) {
            --f.cursor.line;
            f.cursor.col = f.buffer.line_length(f.cursor.line);
        }
    }

    f.cursor.target_col = static_cast<float>(f.cursor.col);
    if (!select) f.cursor.clear_selection();
}

void CodeEditor::move_cursor_right(OpenFile& f, bool select, bool word) {
    if (select) f.cursor.start_selection();

    if (word) {
        auto& line = f.buffer.lines[f.cursor.line];
        int c = f.cursor.col;
        int len = static_cast<int>(line.size());
        if (c < len) {
            while (c < len && is_word_char(line[c])) ++c;
            while (c < len && !is_word_char(line[c])) ++c;
            f.cursor.col = c;
        } else if (f.cursor.line + 1 < f.buffer.line_count()) {
            ++f.cursor.line;
            f.cursor.col = 0;
        }
    } else {
        if (f.cursor.col < f.buffer.line_length(f.cursor.line)) {
            ++f.cursor.col;
        } else if (f.cursor.line + 1 < f.buffer.line_count()) {
            ++f.cursor.line;
            f.cursor.col = 0;
        }
    }

    f.cursor.target_col = static_cast<float>(f.cursor.col);
    if (!select) f.cursor.clear_selection();
}

void CodeEditor::move_cursor_up(OpenFile& f, bool select) {
    if (select) f.cursor.start_selection();
    if (f.cursor.line > 0) {
        --f.cursor.line;
        f.cursor.col = std::min(static_cast<int>(f.cursor.target_col),
                                f.buffer.line_length(f.cursor.line));
    }
    if (!select) f.cursor.clear_selection();
}

void CodeEditor::move_cursor_down(OpenFile& f, bool select) {
    if (select) f.cursor.start_selection();
    if (f.cursor.line + 1 < f.buffer.line_count()) {
        ++f.cursor.line;
        f.cursor.col = std::min(static_cast<int>(f.cursor.target_col),
                                f.buffer.line_length(f.cursor.line));
    }
    if (!select) f.cursor.clear_selection();
}

void CodeEditor::move_cursor_home(OpenFile& f, bool select) {
    if (select) f.cursor.start_selection();

    // Smart home: first go to first non-whitespace, then to col 0
    auto& line = f.buffer.lines[f.cursor.line];
    int first_non_ws = 0;
    while (first_non_ws < static_cast<int>(line.size()) &&
           (line[first_non_ws] == ' ' || line[first_non_ws] == '\t')) {
        ++first_non_ws;
    }
    f.cursor.col = (f.cursor.col == first_non_ws) ? 0 : first_non_ws;
    f.cursor.target_col = static_cast<float>(f.cursor.col);
    if (!select) f.cursor.clear_selection();
}

void CodeEditor::move_cursor_end(OpenFile& f, bool select) {
    if (select) f.cursor.start_selection();
    f.cursor.col = f.buffer.line_length(f.cursor.line);
    f.cursor.target_col = static_cast<float>(f.cursor.col);
    if (!select) f.cursor.clear_selection();
}

// ===========================================================================
// Editing operations
// ===========================================================================

void CodeEditor::handle_backspace(OpenFile& f, bool word) {
    push_undo(f);
    if (f.cursor.has_selection()) {
        delete_selection(f);
    } else if (word) {
        // Delete word before cursor
        int old_col = f.cursor.col;
        int old_line = f.cursor.line;
        move_cursor_left(f, false, true);
        f.buffer.delete_range(f.cursor.line, f.cursor.col, old_line, old_col);
    } else {
        f.buffer.delete_char_before(f.cursor.line, f.cursor.col, f.cursor.line);
    }
    f.cursor.clear_selection();
    f.cursor.target_col = static_cast<float>(f.cursor.col);
    f.dirty = true;
    f.invalidate_from(f.cursor.line);
}

void CodeEditor::handle_delete(OpenFile& f, bool word) {
    push_undo(f);
    if (f.cursor.has_selection()) {
        delete_selection(f);
    } else if (word) {
        int old_col = f.cursor.col;
        int old_line = f.cursor.line;
        // Find end of word
        auto& line = f.buffer.lines[f.cursor.line];
        int c = old_col;
        int len = static_cast<int>(line.size());
        while (c < len && is_word_char(line[c])) ++c;
        while (c < len && !is_word_char(line[c])) ++c;
        f.buffer.delete_range(old_line, old_col, old_line, c);
    } else {
        f.buffer.delete_char(f.cursor.line, f.cursor.col);
    }
    f.cursor.clear_selection();
    f.dirty = true;
    f.invalidate_from(f.cursor.line);
}

void CodeEditor::handle_enter(OpenFile& f) {
    push_undo(f);
    if (f.cursor.has_selection()) {
        delete_selection(f);
    }
    f.buffer.insert_newline(f.cursor.line, f.cursor.col);
    ++f.cursor.line;
    // cursor col = length of auto-indent
    auto& new_line = f.buffer.lines[f.cursor.line];
    int indent = 0;
    while (indent < static_cast<int>(new_line.size()) &&
           (new_line[indent] == ' ' || new_line[indent] == '\t')) {
        ++indent;
    }
    f.cursor.col = indent;
    f.cursor.clear_selection();
    f.cursor.target_col = static_cast<float>(f.cursor.col);
    f.dirty = true;
    f.invalidate_from(f.cursor.line - 1);
}

void CodeEditor::handle_tab(OpenFile& f, bool shift) {
    push_undo(f);

    if (f.cursor.has_selection()) {
        // Indent/outdent selected lines
        int sl, sc, el, ec;
        f.cursor.selection_ordered(sl, sc, el, ec);
        for (int line = sl; line <= el; ++line) {
            if (shift) {
                // Remove leading 4 spaces or 1 tab
                auto& l = f.buffer.lines[line];
                if (!l.empty() && l[0] == '\t') {
                    l.erase(0, 1);
                } else {
                    int spaces = 0;
                    while (spaces < 4 && spaces < static_cast<int>(l.size()) && l[spaces] == ' ') {
                        ++spaces;
                    }
                    l.erase(0, spaces);
                }
            } else {
                f.buffer.lines[line].insert(0, "    ");
            }
        }
        f.dirty = true;
        f.invalidate_from(sl);
    } else {
        if (shift) return; // no-op
        // Insert 4 spaces
        f.buffer.insert_text(f.cursor.line, f.cursor.col, "    ");
        f.cursor.col += 4;
        f.cursor.target_col = static_cast<float>(f.cursor.col);
        f.dirty = true;
        f.invalidate_from(f.cursor.line);
    }
}

void CodeEditor::handle_char(OpenFile& f, ImWchar c) {
    push_undo(f);
    if (f.cursor.has_selection()) {
        delete_selection(f);
    }
    // Convert ImWchar to char (ASCII range for now)
    if (c < 128) {
        f.buffer.insert_char(f.cursor.line, f.cursor.col, static_cast<char>(c));
        ++f.cursor.col;
    }
    f.cursor.clear_selection();
    f.cursor.target_col = static_cast<float>(f.cursor.col);
    f.dirty = true;
    f.invalidate_from(f.cursor.line);
}

void CodeEditor::delete_selection(OpenFile& f) {
    if (!f.cursor.has_selection()) return;
    int sl, sc, el, ec;
    f.cursor.selection_ordered(sl, sc, el, ec);
    f.buffer.delete_range(sl, sc, el, ec);
    f.cursor.line = sl;
    f.cursor.col = sc;
    f.cursor.clear_selection();
    f.cursor.target_col = static_cast<float>(f.cursor.col);
}

std::string CodeEditor::get_selected_text(const OpenFile& f) const {
    if (!f.cursor.has_selection()) return {};
    int sl, sc, el, ec;
    f.cursor.selection_ordered(sl, sc, el, ec);

    if (sl == el) {
        auto& line = f.buffer.lines[sl];
        sc = std::min(sc, static_cast<int>(line.size()));
        ec = std::min(ec, static_cast<int>(line.size()));
        return line.substr(sc, ec - sc);
    }

    std::string result;
    // First line
    result += f.buffer.lines[sl].substr(sc);
    // Middle lines
    for (int i = sl + 1; i < el; ++i) {
        result += '\n';
        result += f.buffer.lines[i];
    }
    // Last line
    result += '\n';
    result += f.buffer.lines[el].substr(0, ec);
    return result;
}

void CodeEditor::select_all(OpenFile& f) {
    f.cursor.sel_line = 0;
    f.cursor.sel_col = 0;
    f.cursor.line = f.buffer.line_count() - 1;
    f.cursor.col = f.buffer.line_length(f.cursor.line);
}

// ===========================================================================
// Undo/redo (snapshot-based, debounced)
// ===========================================================================

void CodeEditor::push_undo(OpenFile& f) {
    // Debounce: don't snapshot more than once per 0.3s
    float now = static_cast<float>(ImGui::GetTime());
    if (now - f.undo_timer < 0.3f && !f.undo_stack.empty()) return;
    f.undo_timer = now;

    // Truncate redo history
    if (f.undo_index + 1 < static_cast<int>(f.undo_stack.size())) {
        f.undo_stack.erase(f.undo_stack.begin() + f.undo_index + 1, f.undo_stack.end());
    }

    f.undo_stack.push_back({f.buffer.get_text(), f.cursor.line, f.cursor.col});
    f.undo_index = static_cast<int>(f.undo_stack.size()) - 1;

    // Cap undo history
    constexpr int MAX_UNDO = 200;
    if (static_cast<int>(f.undo_stack.size()) > MAX_UNDO) {
        f.undo_stack.erase(f.undo_stack.begin());
        --f.undo_index;
    }
}

void CodeEditor::undo(OpenFile& f) {
    if (f.undo_index <= 0) return;
    // Save current state if we're at the top
    if (f.undo_index == static_cast<int>(f.undo_stack.size()) - 1) {
        if (f.undo_stack.back().text != f.buffer.get_text()) {
            f.undo_stack.push_back({f.buffer.get_text(), f.cursor.line, f.cursor.col});
            // undo_index still points to the previous entry
        }
    }
    --f.undo_index;
    auto& entry = f.undo_stack[f.undo_index];
    f.buffer.set_text(entry.text);
    f.cursor.line = entry.cursor_line;
    f.cursor.col = entry.cursor_col;
    f.cursor.clear_selection();
    f.dirty = true;
    f.rebuild_line_states();
}

void CodeEditor::redo(OpenFile& f) {
    if (f.undo_index + 1 >= static_cast<int>(f.undo_stack.size())) return;
    ++f.undo_index;
    auto& entry = f.undo_stack[f.undo_index];
    f.buffer.set_text(entry.text);
    f.cursor.line = entry.cursor_line;
    f.cursor.col = entry.cursor_col;
    f.cursor.clear_selection();
    f.dirty = true;
    f.rebuild_line_states();
}

// ===========================================================================
// Scroll
// ===========================================================================

void CodeEditor::ensure_cursor_visible(OpenFile& f, float visible_lines) {
    float cursor_line = static_cast<float>(f.cursor.line);
    if (cursor_line < f.scroll_y + 1.0f) {
        f.scroll_y = std::max(0.0f, cursor_line - 1.0f);
    }
    if (cursor_line > f.scroll_y + visible_lines - 2.0f) {
        f.scroll_y = cursor_line - visible_lines + 2.0f;
    }
}

// ===========================================================================
// Rendering
// ===========================================================================

void CodeEditor::render_editor(OpenFile& f) {
    auto* draw_list = ImGui::GetWindowDrawList();
    ImVec2 region = ImGui::GetContentRegionAvail();

    // Compute gutter width (line number column)
    int total_lines = f.buffer.line_count();
    int digits = 1;
    { int n = total_lines; while (n >= 10) { n /= 10; ++digits; } }
    gutter_width_ = (static_cast<float>(digits) + 1.5f) * char_width_;

    float visible_lines = region.y / line_height_;
    ensure_cursor_visible(f, visible_lines);

    // Handle scroll wheel (Ctrl+wheel = zoom, plain wheel = scroll)
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f && ImGui::GetIO().KeyCtrl) {
            font_scale_ = std::clamp(font_scale_ + wheel * 0.1f, 0.5f, 3.0f);
            update_font_metrics();
        } else if (wheel != 0.0f) {
            f.scroll_y -= wheel * 3.0f;
            f.scroll_y = std::clamp(f.scroll_y, 0.0f,
                                     std::max(0.0f, static_cast<float>(total_lines) - visible_lines + 1.0f));
        }
    }

    // Mouse click → set cursor position
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 mouse = ImGui::GetMousePos();

    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int click_line = static_cast<int>((mouse.y - origin.y) / line_height_ + f.scroll_y);
        int click_col = static_cast<int>((mouse.x - origin.x - gutter_width_) / char_width_);
        click_line = std::clamp(click_line, 0, f.buffer.line_count() - 1);
        click_col = std::clamp(click_col, 0, f.buffer.line_length(click_line));

        bool shift = ImGui::GetIO().KeyShift;
        if (shift) {
            f.cursor.start_selection();
        } else {
            f.cursor.clear_selection();
        }
        f.cursor.line = click_line;
        f.cursor.col = click_col;
        f.cursor.target_col = static_cast<float>(click_col);
    }

    // Mouse drag → extend selection
    if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        int drag_line = static_cast<int>((mouse.y - origin.y) / line_height_ + f.scroll_y);
        int drag_col = static_cast<int>((mouse.x - origin.x - gutter_width_) / char_width_);
        drag_line = std::clamp(drag_line, 0, f.buffer.line_count() - 1);
        drag_col = std::clamp(drag_col, 0, f.buffer.line_length(drag_line));

        f.cursor.start_selection();
        f.cursor.line = drag_line;
        f.cursor.col = drag_col;
    }

    int first_visible = static_cast<int>(f.scroll_y);
    int last_visible = std::min(first_visible + static_cast<int>(visible_lines) + 1,
                                total_lines);

    static const ImU32 BG_COLOR         = srgb_u32(17, 18, 20);       // #111214 — darker than WindowBg
    static const ImU32 GUTTER_BG        = srgb_u32(13, 14, 16);       // even darker
    static const ImU32 CURRENT_LINE_BG  = srgb_u32(30, 31, 34);       // #1E1F22 — subtle lift
    static const ImU32 SELECTION_BG     = srgb_u32(55, 65, 85);       // blue-gray
    static const ImU32 CURSOR_COLOR     = srgb_u32(220, 223, 228);    // #DCE0E4
    static const ImU32 LINE_NUM_COLOR   = srgb_u32(92, 99, 112);      // #5C6370
    static const ImU32 LINE_NUM_ACTIVE  = srgb_u32(171, 178, 191);    // brighter for current

    // Background
    draw_list->AddRectFilled(origin, {origin.x + region.x, origin.y + region.y}, BG_COLOR);
    // Gutter background
    draw_list->AddRectFilled(origin, {origin.x + gutter_width_, origin.y + region.y}, GUTTER_BG);

    // Selection ranges
    if (f.cursor.has_selection()) {
        int sl, sc, el, ec;
        f.cursor.selection_ordered(sl, sc, el, ec);
        for (int row = std::max(first_visible, sl); row <= std::min(last_visible - 1, el); ++row) {
            float y = origin.y + (static_cast<float>(row) - f.scroll_y) * line_height_;
            int c_start = (row == sl) ? sc : 0;
            int c_end = (row == el) ? ec : f.buffer.line_length(row);
            if (row != el) ++c_end; // extend to show newline selection
            float x_start = origin.x + gutter_width_ + static_cast<float>(c_start) * char_width_;
            float x_end = origin.x + gutter_width_ + static_cast<float>(c_end) * char_width_;
            draw_list->AddRectFilled({x_start, y}, {x_end, y + line_height_}, SELECTION_BG);
        }
    }

    // Current line highlight
    {
        float y = origin.y + (static_cast<float>(f.cursor.line) - f.scroll_y) * line_height_;
        if (y >= origin.y && y < origin.y + region.y) {
            draw_list->AddRectFilled(
                {origin.x + gutter_width_, y},
                {origin.x + region.x, y + line_height_},
                CURRENT_LINE_BG);
        }
    }

    // Draw lines
    std::vector<Token> tokens;
    for (int row = first_visible; row < last_visible; ++row) {
        float y = origin.y + (static_cast<float>(row) - f.scroll_y) * line_height_;
        float text_x = origin.x + gutter_width_;

        // Line number
        {
            char num_buf[16];
            int d = std::min(digits, 10); // cap to prevent format-truncation warning
            (void)snprintf(num_buf, sizeof(num_buf), "%*d", d, row + 1);
            ImU32 num_color = (row == f.cursor.line) ? LINE_NUM_ACTIVE : LINE_NUM_COLOR;
            float num_x = origin.x + (gutter_width_ - static_cast<float>(digits) * char_width_) - char_width_ * 0.5f;
            draw_list->AddText(mono_font_, mono_font_->LegacySize * font_scale_, {num_x, y}, num_color, num_buf);
        }

        // Tokenize and draw colored text
        auto& line = f.buffer.lines[row];
        if (line.empty()) continue;

        LineState prev_state = (row > 0 && row - 1 < static_cast<int>(f.line_states.size()))
                                ? f.line_states[row - 1] : LineState{};
        (void)tokenize_line(line, f.language, prev_state, tokens);

        for (auto& tok : tokens) {
            if (tok.start >= tok.end) continue;
            float x = text_x + static_cast<float>(tok.start) * char_width_;
            ImU32 color = token_color(tok.type);

            // We need a null-terminated substring
            int len = tok.end - tok.start;
            const char* start = line.c_str() + tok.start;
            draw_list->AddText(mono_font_, mono_font_->LegacySize * font_scale_,
                              {x, y}, color, start, start + len);
        }
    }

    // Cursor (blinking bar)
    {
        float cursor_y = origin.y + (static_cast<float>(f.cursor.line) - f.scroll_y) * line_height_;
        float cursor_x = origin.x + gutter_width_ + static_cast<float>(f.cursor.col) * char_width_;

        // Blink
        bool blink_on = (static_cast<int>(ImGui::GetTime() * 2.0f) % 2) == 0;
        if (blink_on && cursor_y >= origin.y && cursor_y < origin.y + region.y) {
            draw_list->AddRectFilled(
                {cursor_x, cursor_y},
                {cursor_x + 2.0f, cursor_y + line_height_},
                CURSOR_COLOR);
        }
    }

    // Reserve space for ImGui scrolling
    ImGui::Dummy({gutter_width_ + static_cast<float>(100) * char_width_,
                  static_cast<float>(total_lines) * line_height_});

    // Tell ImGui we want keyboard input
    if (ImGui::IsWindowFocused()) {
        ImGui::GetIO().WantTextInput = true;
        ImGui::GetIO().WantCaptureKeyboard = true;
    }
}

} // namespace fjell
