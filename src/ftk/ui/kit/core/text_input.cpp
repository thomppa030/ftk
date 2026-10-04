#include "ftk/ui/kit/core/text_input.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

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
// and Enter unless the field keeps it.
void take_keys(Editor& editor, TextEditing& state, TextInput& out, const TextInputOptions& options) {
    core::Context& context = ui();
    const Modifiers mods = context.modifiers();
    const bool shortcut = mods.ctrl || mods.super;
    const auto key = [&](Key k) { return context.key_repeated(k); };

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
    if (key(Key::Home)) editor.move(0, mods.shift);
    if (key(Key::End)) editor.move(state.text.size(), mods.shift);
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
        if (context.key_pressed(Key::V)) editor.insert(one_line(context.clipboard()));
        if (key(Key::Z)) mods.shift ? editor.redo() : editor.undo();
        if (key(Key::Y)) editor.redo();
    }
    if (!context.typed().empty()) editor.insert(one_line(context.typed()));

    if (context.key_pressed(Key::Enter)) {
        out.entered = true;
        if (!options.enter_keeps) context.clear_focus();
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

    if (focus.gained) {
        // The field that had the keyboard keeps what it held until it is
        // drawn again and told.
        if (k.editing.id != 0 && k.editing.id != id) k.left = std::move(k.editing);
        k.editing = {.id = id, .text = value, .initial = value, .moved = context.time()};
        // Given by a click, the caret goes where it landed; given by the
        // keyboard, or asked to, the whole text is selected.
        if (in.pressed && !options.select_all) {
            const float x = in.position.x - origin_x;
            k.editing.caret = k.editing.anchor = context.text().caret_at(value, text, x);
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

    const bool editing = focus.focused && k.editing.id == id;
    if (editing) {
        TextEditing& state = k.editing;
        Editor editor(state);
        const std::size_t caret_before = state.caret;
        const std::string text_before = state.text;
        // The mouse places the caret, and a drag or a double click selects.
        if (in.pressed && !out.activated) {
            const std::size_t at = context.text().caret_at(state.text, text, in.position.x - origin_x + state.scroll);
            editor.move(at, context.modifiers().shift);
        }
        if (in.double_clicked) {
            state.anchor = word_left(state.text, state.caret);
            std::size_t right = state.caret;
            while (right < state.text.size() && !space(state.text[right])) right = next(state.text, right);
            state.caret = right;
        } else if (in.held && in.dragging) {
            editor.move(context.text().caret_at(state.text, text, in.position.x - origin_x + state.scroll), true);
        }
        take_keys(editor, state, out, options);
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

    // The frame, and the text scrolled to keep the caret in sight.
    draw_frame(rect, in, editing);
    core::DrawList& dl = context.draw_list();
    const core::Rect inside{.min = {origin_x, rect.min.y},
                            .max = {rect.max.x - style.frame_padding.x - options.trail, rect.max.y}};
    dl.push_clip(inside.min, inside.max);
    if (editing) {
        TextEditing& state = k.editing;
        // What an input method is composing shows at the caret, underlined.
        const std::string_view composing = context.composition();
        std::string shown_text = state.text;
        std::size_t caret = state.caret;
        if (!composing.empty()) {
            shown_text.insert(state.caret, composing);
            caret = state.caret + static_cast<std::size_t>(std::clamp(context.composition_cursor(), 0,
                                                                      static_cast<int>(composing.size())));
        }
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
        // The caret blinks, steady for a moment after it moves.
        const double since = context.time() - state.moved;
        const double phase = std::fmod(since, BLINK);
        if (phase <= BLINK_ON) {
            const float x = std::round(x0 + caret_x);
            dl.line({x, text_y}, {x, text_y + text.size}, faded(text_colour()));
        }
        context.request_frame_at(state.moved + (std::floor(since / BLINK) * BLINK) + (phase <= BLINK_ON ? BLINK_ON : BLINK));
        context.set_text_input_area({.min = {x0 + caret_x, rect.min.y}, .max = {x0 + caret_x + 1.0f, rect.max.y}});
    } else if (value.empty() && options.hint != nullptr) {
        draw_text({origin_x, text_y}, theme::text_disabled(), options.hint);
    } else {
        draw_text({origin_x, text_y}, text_colour(), value);
    }
    dl.pop_clip();

    Item& item = k.item;
    item.keyboard = true;
    item.focused = focus.focused;
    item.focus_gained = focus.gained;
    item.focus_lost = focus.lost;
    item.edited = out.edited;
    return out;
}

} // namespace ftk::ui::corekit
