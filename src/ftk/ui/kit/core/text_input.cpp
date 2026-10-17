#include "ftk/ui/kit/core/text_input.hpp"

#include "ftk/text/line_break.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace ftk::ui::corekit {

namespace {

// The caret is on this long of each blink, out of the whole.
constexpr double BLINK = 1.2;
constexpr double BLINK_ON = 0.8;

bool continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// The character boundaries either side of `at`.
std::size_t next(const std::string& text, std::size_t at) {
    if (at >= text.size()) return text.size();
    ++at;
    while (at < text.size() && continuation(text[at])) ++at;
    return at;
}

std::size_t previous(const std::string& text, std::size_t at) {
    if (at == 0) return 0;
    --at;
    while (at > 0 && continuation(text[at])) --at;
    return at;
}

bool space(char c) {
    return c == ' ' || c == '\t';
}

// A word's ends either side of `at`: past the spaces, then the word.
std::size_t word_left(const std::string& text, std::size_t at) {
    while (at > 0 && space(text[at - 1])) at = previous(text, at);
    while (at > 0 && !space(text[at - 1])) at = previous(text, at);
    return at;
}

std::size_t word_right(const std::string& text, std::size_t at) {
    while (at < text.size() && !space(text[at])) at = next(text, at);
    while (at < text.size() && space(text[at])) at = next(text, at);
    return at;
}

// The text a field can hold: one line, no control characters.
std::string one_line(std::string_view text) {
    std::string kept;
    kept.reserve(text.size());
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) kept += c;
    }
    return kept;
}

// The text an area can hold: lines ended by '\n' alone, no other control
// characters.
std::string lines_of(std::string_view text) {
    std::string kept;
    kept.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            // "\r\n" is one line end, and a lone '\r' one as well.
            if (i + 1 >= text.size() || text[i + 1] != '\n') kept += '\n';
        } else if (c == '\n' || (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F)) {
            kept += c;
        }
    }
    return kept;
}

// An area's text laid into lines at its width.
struct Lines {
    std::vector<text::Line> lines;

    // The line the caret before byte `at` is on. A caret where a line wraps
    // is at the start of the next.
    [[nodiscard]] std::size_t of(std::size_t at) const {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (at < lines[i].end) return i;
        }
        return lines.size() - 1;
    }

    // The furthest along line `i` the caret goes: before its newline, or
    // before the character it wraps at; on the last, the end of the text.
    [[nodiscard]] std::size_t last(std::size_t i, const std::string& text) const {
        const text::Line& line = lines[i];
        if (i + 1 == lines.size()) return line.end;
        if (line.end > line.begin && text[line.end - 1] == '\n') return line.end - 1;
        return std::max(line.begin, previous(text, line.end));
    }

    // Line `i`'s text without its newline, along which a caret is placed.
    [[nodiscard]] std::string_view text_of(std::size_t i, std::string_view text) const {
        const text::Line& line = lines[i];
        std::size_t end = line.end;
        if (end > line.begin && text[end - 1] == '\n') --end;
        return text.substr(line.begin, end - line.begin);
    }
};

// `text` broken into lines at `width` as it is drawn, with the empty line
// that empty text is, or that a final newline starts.
Lines lay_out(std::string_view text, core::TextStyle style, float width) {
    Lines laid{.lines = ui().text().break_lines(text, style, width)};
    if (text.empty() || text.back() == '\n') {
        laid.lines.push_back({.begin = text.size(), .end = text.size()});
    }
    return laid;
}

// The lines a text area's keys move the caret through.
struct Area {
    const Lines& lines;
    core::TextStyle style;
};

class Editor {
public:
    explicit Editor(TextEditing& state) : s_(state) {}

    [[nodiscard]] bool selected() const { return s_.caret != s_.anchor; }
    [[nodiscard]] std::size_t start() const { return std::min(s_.caret, s_.anchor); }
    [[nodiscard]] std::size_t end() const { return std::max(s_.caret, s_.anchor); }

    // The caret to `at`, the selection kept from the anchor when `extend`.
    void move(std::size_t at, bool extend) {
        s_.caret = std::min(at, s_.text.size());
        if (!extend) s_.anchor = s_.caret;
    }

    void insert(std::string_view text) {
        if (text.empty() && !selected()) return;
        remember();
        const std::size_t from = start();
        s_.text.replace(from, end() - from, text);
        s_.caret = s_.anchor = from + text.size();
        s_.edited = true;
    }

    void erase(std::size_t from, std::size_t to) {
        if (from >= to) return;
        remember();
        s_.text.erase(from, to - from);
        s_.caret = s_.anchor = from;
        s_.edited = true;
    }

    void undo() { step(s_.undo, s_.redo); }
    void redo() { step(s_.redo, s_.undo); }

    [[nodiscard]] std::string selection() const { return s_.text.substr(start(), end() - start()); }

private:
    void remember() {
        s_.undo.emplace_back(s_.text, s_.caret);
        s_.redo.clear();
    }

    void step(std::vector<std::pair<std::string, std::size_t>>& from,
              std::vector<std::pair<std::string, std::size_t>>& to) {
        if (from.empty()) return;
        to.emplace_back(s_.text, s_.caret);
        s_.text = std::move(from.back().first);
        s_.caret = s_.anchor = std::min(from.back().second, s_.text.size());
        from.pop_back();
        s_.edited = true;
    }

    TextEditing& s_;
};

// The keys the field takes while it has the keyboard; Escape lets it go,
// and Enter unless the field keeps it. In an area, with its lines, Up and
// Down go from line to line, Home and End to a line's ends, and Enter
// starts a new line, Ctrl Enter finishing instead.
void take_keys(Editor& editor, TextEditing& state, TextInput& out, const TextInputOptions& options,
               const Area* area = nullptr) {
    core::Context& context = ui();
    const Modifiers mods = context.modifiers();
    const bool shortcut = mods.ctrl || mods.super;
    const auto key = [&](Key k) { return context.key_repeated(k); };

    // Moved through the lines before anything edits the text they are of.
    if (area != nullptr) {
        const Lines& lines = area->lines;
        const std::size_t line = lines.of(state.caret);
        const std::size_t begin = lines.lines[line].begin;
        const bool up = key(Key::Up);
        if (up || key(Key::Down)) {
            const std::size_t to = up ? line - std::min<std::size_t>(line, 1) : line + 1;
            if (up && line == 0) {
                editor.move(0, mods.shift);
            } else if (!up && to >= lines.lines.size()) {
                editor.move(state.text.size(), mods.shift);
            } else {
                // Straight up or down from where the caret stands.
                core::TextSystem& system = context.text();
                const float x = system.caret_x(lines.text_of(line, state.text), area->style, state.caret - begin);
                const std::size_t along = system.caret_at(lines.text_of(to, state.text), area->style, x);
                editor.move(std::min(lines.lines[to].begin + along, lines.last(to, state.text)), mods.shift);
            }
        }
        if (key(Key::Home)) editor.move(shortcut ? 0 : begin, mods.shift);
        if (key(Key::End)) editor.move(shortcut ? state.text.size() : lines.last(line, state.text), mods.shift);
    } else {
        if (key(Key::Home)) editor.move(0, mods.shift);
        if (key(Key::End)) editor.move(state.text.size(), mods.shift);
    }

    if (key(Key::Left)) {
        if (editor.selected() && !mods.shift) {
            editor.move(editor.start(), false);
        } else {
            editor.move(shortcut ? word_left(state.text, state.caret) : previous(state.text, state.caret), mods.shift);
        }
    }
    if (key(Key::Right)) {
        if (editor.selected() && !mods.shift) {
            editor.move(editor.end(), false);
        } else {
            editor.move(shortcut ? word_right(state.text, state.caret) : next(state.text, state.caret), mods.shift);
        }
    }
    if (key(Key::Backspace)) {
        if (editor.selected()) {
            editor.erase(editor.start(), editor.end());
        } else {
            const std::size_t to = state.caret;
            editor.erase(shortcut ? word_left(state.text, to) : previous(state.text, to), to);
        }
    }
    if (key(Key::Delete)) {
        if (editor.selected()) {
            editor.erase(editor.start(), editor.end());
        } else {
            const std::size_t from = state.caret;
            editor.erase(from, shortcut ? word_right(state.text, from) : next(state.text, from));
        }
    }
    if (shortcut) {
        if (context.key_pressed(Key::A)) {
            state.anchor = 0;
            state.caret = state.text.size();
        }
        if (context.key_pressed(Key::C) && editor.selected()) context.set_clipboard(editor.selection());
        if (context.key_pressed(Key::X) && editor.selected()) {
            context.set_clipboard(editor.selection());
            editor.erase(editor.start(), editor.end());
        }
        if (context.key_pressed(Key::V)) {
            editor.insert(area != nullptr ? lines_of(context.clipboard()) : one_line(context.clipboard()));
        }
        if (key(Key::Z)) mods.shift ? editor.redo() : editor.undo();
        if (key(Key::Y)) editor.redo();
    }
    if (!context.typed().empty()) editor.insert(one_line(context.typed()));

    if (context.key_pressed(Key::Enter)) {
        if (area != nullptr && !shortcut) {
            editor.insert("\n");
        } else {
            out.entered = true;
            if (!options.enter_keeps) context.clear_focus();
        }
    } else if (context.key_pressed(Key::Escape)) {
        out.escaped = true;
        switch (options.escape) {
        case EscapeDoes::Revert:
            state.text = state.initial;
            state.cancelled = true;
            break;
        case EscapeDoes::Clear:
            if (!state.text.empty()) editor.erase(0, state.text.size());
            break;
        case EscapeDoes::Keep: break;
        }
        context.clear_focus();
    }
}

// The keyboard taken and let go, as every text box does: the box's editing
// state made as it takes it, the caret where `caret_at` puts the click that
// gave it, and the finished text handed back to `value` as it lets go.
template <typename CaretAt>
void take_keyboard(Id id, const core::Interaction& in, const core::Focus& focus, std::string& value,
                   const TextInputOptions& options, TextInput& out, CaretAt&& caret_at) {
    core::Context& context = ui();
    Kit& k = kit();
    if (focus.gained) {
        // The field that had the keyboard keeps what it held until it is
        // drawn again and told.
        if (k.editing.id != 0 && k.editing.id != id) k.left = std::move(k.editing);
        k.editing = {.id = id, .text = value, .initial = value, .moved = context.time()};
        // Given by a click, the caret goes where it landed; given by the
        // keyboard, or asked to, the whole text is selected, or the caret
        // goes after it where the box is filled in to be added to.
        if (in.pressed && !options.select_all) {
            k.editing.caret = k.editing.anchor = caret_at(value, in.position);
        } else if (!in.pressed && options.caret_at_end) {
            k.editing.caret = k.editing.anchor = value.size();
        } else {
            k.editing.anchor = 0;
            k.editing.caret = value.size();
        }
        out.activated = true;
    }
    if (focus.lost) {
        TextEditing* state = k.editing.id == id ? &k.editing : (k.left.id == id ? &k.left : nullptr);
        if (state != nullptr) {
            const std::string finished = state->cancelled ? state->initial : state->text;
            out.changed = finished != value;
            value = finished;
            out.edited = state->edited && !state->cancelled && finished != state->initial;
            out.cancelled = state->cancelled;
            *state = {};
        }
        out.deactivated = true;
    }
}

// While the box has the keyboard: the mouse places the caret where
// `caret_at` says and a drag or a double click selects, the keys edit, and `value`
// follows the text.
template <typename CaretAt>
void edit_text(const core::Interaction& in, std::string& value, const TextInputOptions& options, TextInput& out,
               const Area* area, CaretAt&& caret_at) {
    core::Context& context = ui();
    TextEditing& state = kit().editing;
    Editor editor(state);
    const std::size_t caret_before = state.caret;
    const std::string text_before = state.text;
    if (in.pressed && !out.activated) editor.move(caret_at(state.text, in.position), context.modifiers().shift);
    if (in.double_clicked) {
        state.anchor = word_left(state.text, state.caret);
        std::size_t right = state.caret;
        while (right < state.text.size() && !space(state.text[right]) && state.text[right] != '\n') {
            right = next(state.text, right);
        }
        state.caret = right;
    } else if (in.held && in.dragging) {
        editor.move(caret_at(state.text, in.position), true);
    }
    take_keys(editor, state, out, options, area);
    if (state.text != text_before || state.caret != caret_before) state.moved = context.time();
    if (state.text != value) {
        value = state.text;
        out.changed = true;
    }
    if (state.cancelled) {
        value = state.initial;
        out.cancelled = true;
    }
}

// The caret, blinking, steady for a moment after it moves, from `top`
// `height` long; and where an input method's window goes.
void draw_caret(const TextEditing& state, float x, float top, float height, core::Rect field) {
    core::Context& context = ui();
    const double since = context.time() - state.moved;
    const double phase = std::fmod(since, BLINK);
    if (phase <= BLINK_ON) {
        const float at = std::round(x);
        context.draw_list().line({at, top}, {at, top + height}, faded(text_colour()));
    }
    context.request_frame_at(state.moved + (std::floor(since / BLINK) * BLINK) + (phase <= BLINK_ON ? BLINK_ON : BLINK));
    context.set_text_input_area({.min = {x, field.min.y}, .max = {x + 1.0f, field.max.y}});
}

// What an input method is composing shown at the caret: `shown` is the
// text with it put in, and the caret moved to the input method's.
struct Composed {
    std::string shown;
    std::string_view composing;
    std::size_t caret{0};
};

Composed composed(const TextEditing& state) {
    core::Context& context = ui();
    Composed c{.shown = state.text, .composing = context.composition(), .caret = state.caret};
    if (!c.composing.empty()) {
        c.shown.insert(state.caret, c.composing);
        c.caret = state.caret + static_cast<std::size_t>(std::clamp(context.composition_cursor(), 0,
                                                                    static_cast<int>(c.composing.size())));
    }
    return c;
}

void finish_item(const core::Focus& focus, const TextInput& out) {
    Item& item = kit().item;
    item.keyboard = true;
    item.focused = focus.focused;
    item.focus_gained = focus.gained;
    item.focus_lost = focus.lost;
    item.edited = out.edited;
}

} // namespace

TextInput text_input(Id id, core::Rect rect, std::string& value, const TextInputOptions& options) {
    core::Context& context = ui();
    Kit& k = kit();
    const Style& style = k.style;
    const core::TextStyle text = text_style();
    const float origin_x = rect.min.x + style.frame_padding.x + options.lead;
    const float text_y = rect.min.y + std::floor((rect.height() - text.size) * 0.5f);

    const core::Interaction in = place(id, rect, {});
    const core::Focus focus = context.focusable(id, {.text = true});
    TextInput out;
    // Where along the text a point is, the text scrolled as it is shown.
    const auto caret_for = [&](const std::string& value_text, glm::vec2 at) {
        return context.text().caret_at(value_text, text, at.x - origin_x + k.editing.scroll);
    };
    take_keyboard(id, in, focus, value, options, out, caret_for);
    const bool editing = focus.focused && k.editing.id == id;
    if (editing) edit_text(in, value, options, out, nullptr, caret_for);

    // The frame, and the text scrolled to keep the caret in sight.
    draw_frame(rect, in, editing);
    core::DrawList& dl = context.draw_list();
    const core::Rect inside{.min = {origin_x, rect.min.y},
                            .max = {rect.max.x - style.frame_padding.x - options.trail, rect.max.y}};
    dl.push_clip(inside.min, inside.max);
    if (editing) {
        TextEditing& state = k.editing;
        // What an input method is composing shows at the caret, underlined.
        const Composed shown = composed(state);
        const std::string& shown_text = shown.shown;
        const std::string_view composing = shown.composing;
        const std::size_t caret = shown.caret;
        core::TextSystem& system = context.text();
        const float caret_x = system.caret_x(shown_text, text, caret);
        const float width = std::max(inside.width(), 0.0f);
        state.scroll = std::clamp(state.scroll, caret_x - width, caret_x);
        state.scroll = std::clamp(state.scroll, 0.0f, std::max(system.measure(shown_text, text).x - width, 0.0f));
        const float x0 = origin_x - state.scroll;

        if (state.caret != state.anchor && composing.empty()) {
            const float a = system.caret_x(state.text, text, std::min(state.caret, state.anchor));
            const float b = system.caret_x(state.text, text, std::max(state.caret, state.anchor));
            dl.rect_filled({x0 + a, text_y}, {x0 + b, text_y + text.size}, faded(theme::accent_dim()));
        }
        draw_text({x0, text_y}, text_colour(), shown_text);
        if (!composing.empty()) {
            const float a = system.caret_x(shown_text, text, state.caret);
            const float b = system.caret_x(shown_text, text, state.caret + composing.size());
            dl.line({x0 + a, text_y + text.size + 1.0f}, {x0 + b, text_y + text.size + 1.0f}, faded(text_colour()));
        }
        draw_caret(state, x0 + caret_x, text_y, text.size, rect);
    } else if (value.empty() && options.hint != nullptr) {
        draw_text({origin_x, text_y}, theme::text_disabled(), options.hint);
    } else {
        draw_text({origin_x, text_y}, text_colour(), value);
    }
    dl.pop_clip();
    finish_item(focus, out);
    return out;
}

TextInput text_area_input(Id id, core::Rect rect, std::string& value, const TextInputOptions& options) {
    core::Context& context = ui();
    Kit& k = kit();
    const Style& style = k.style;
    const core::TextStyle text = text_style();
    core::TextSystem& system = context.text();
    const core::Rect inside{.min = rect.min + style.frame_padding, .max = rect.max - style.frame_padding};
    const float width = std::max(inside.width(), 1.0f);
    const float line_height = system.line_height(text);

    const core::Interaction in = place(id, rect, {});
    const core::Focus focus = context.focusable(id, {.text = true});
    TextInput out;
    // Which line a point is on, the text scrolled as it is shown, and where
    // along that line.
    const auto caret_for = [&](const std::string& value_text, glm::vec2 at) {
        const Lines lines = lay_out(value_text, text, width);
        const float y = at.y - inside.min.y + k.editing.scroll;
        const auto row = static_cast<std::size_t>(std::clamp(std::floor(y / line_height), 0.0f,
                                                             static_cast<float>(lines.lines.size() - 1)));
        const std::size_t along = system.caret_at(lines.text_of(row, value_text), text, at.x - inside.min.x);
        return std::min(lines.lines[row].begin + along, lines.last(row, value_text));
    };
    take_keyboard(id, in, focus, value, options, out, caret_for);
    const bool editing = focus.focused && k.editing.id == id;
    if (editing) {
        const Lines lines = lay_out(k.editing.text, text, width);
        const Area area{.lines = lines, .style = text};
        edit_text(in, value, options, out, &area, caret_for);
    }

    draw_frame(rect, in, editing);
    core::DrawList& dl = context.draw_list();
    dl.push_clip(inside.min, inside.max);
    if (editing) {
        TextEditing& state = k.editing;
        const Composed shown = composed(state);
        const Lines lines = lay_out(shown.shown, text, width);

        // Scrolled to keep the caret's line in sight.
        const std::size_t caret_line = lines.of(shown.caret);
        const float caret_top = static_cast<float>(caret_line) * line_height;
        const float all = static_cast<float>(lines.lines.size()) * line_height;
        state.scroll = std::clamp(state.scroll, caret_top + line_height - inside.height(), caret_top);
        state.scroll = std::clamp(state.scroll, 0.0f, std::max(all - inside.height(), 0.0f));
        const float y0 = inside.min.y - state.scroll;
        const auto x_on = [&](std::size_t line, std::size_t at) {
            return inside.min.x
                 + system.caret_x(lines.text_of(line, shown.shown), text, at - lines.lines[line].begin);
        };

        const std::size_t from = std::min(state.caret, state.anchor);
        const std::size_t to = std::max(state.caret, state.anchor);
        for (std::size_t i = 0; i < lines.lines.size(); ++i) {
            const float top = y0 + static_cast<float>(i) * line_height;
            if (top + line_height < inside.min.y || top > inside.max.y) continue;
            const text::Line& line = lines.lines[i];
            // The selection on this line, a little past its end where it
            // takes the newline too.
            if (from != to && shown.composing.empty() && from < std::max(line.end, line.begin + 1) && to > line.begin) {
                const std::size_t a = std::max(from, line.begin);
                const std::size_t b = std::min(to, line.begin + lines.text_of(i, shown.shown).size());
                const float past = to > b && i + 1 < lines.lines.size() ? theme::GAP_XS : 0.0f;
                dl.rect_filled({x_on(i, a), top}, {x_on(i, b) + past, top + line_height}, faded(theme::accent_dim()));
            }
            draw_text({inside.min.x, top}, text_colour(), lines.text_of(i, shown.shown));
        }
        if (!shown.composing.empty()) {
            const std::size_t end = std::min(state.caret + shown.composing.size(), lines.last(caret_line, shown.shown));
            const float under = y0 + caret_top + line_height;
            dl.line({x_on(caret_line, state.caret), under}, {x_on(caret_line, end), under}, faded(text_colour()));
        }
        draw_caret(state, x_on(caret_line, shown.caret), y0 + caret_top, line_height, rect);
    } else if (value.empty() && options.hint != nullptr) {
        draw_text(inside.min, theme::text_disabled(), options.hint);
    } else {
        const Lines lines = lay_out(value, text, width);
        for (std::size_t i = 0; i < lines.lines.size(); ++i) {
            const float top = inside.min.y + static_cast<float>(i) * line_height;
            if (top > inside.max.y) break;
            draw_text({inside.min.x, top}, text_colour(), lines.text_of(i, value));
        }
    }
    dl.pop_clip();
    finish_item(focus, out);
    return out;
}

} // namespace ftk::ui::corekit
