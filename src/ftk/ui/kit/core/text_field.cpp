#include "ftk/ui/kit/text_field.hpp"

#include "ftk/ui/kit/core/text_input.hpp"
#include "ftk/ui/kit/edit_record.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <cmath>

namespace ftk::ui {

using namespace corekit;

namespace {

// A text field at the cursor, as wide as the next item is to be.
TextInput field(const char* id, std::string& value, const TextInputOptions& options) {
    core::Context& context = ui();
    const core::Rect rect = context.layout().next({item_width(), frame_height()});
    const Id field_id = context.id(id);
    const TextInput in = text_input(field_id, rect, value, options);
    context.semantics(field_id, core::Role::TextField, shown(id), value);
    return in;
}

} // namespace

Edit text_field(const char* id, std::string& value) {
    // `value` is the caller's, put back on every frame but the one the
    // edit is committed on: while it is typed in, the field holds its own.
    const std::string original = value;
    const TextInput in = field(id, value, {});
    Edit edit;
    edit.changed = in.changed;
    edit.committed = in.deactivated && in.edited && value != original;
    if (!edit.committed) value = original;
    detail::track_field(kit().item.id, "\"" + original + "\"", "\"" + value + "\"", in.activated, edit);
    return edit;
}

Edit text_area(const char* id, std::string& value, TextAreaOptions options) {
    core::Context& context = ui();
    const std::string original = value;
    const float lines = static_cast<float>(std::max(options.lines, 1));
    const float height = (lines * context.text().line_height(text_style())) + (2.0f * kit().style.frame_padding.y);
    const core::Rect rect = context.layout().next({item_width(), height});
    const Id field_id = context.id(id);
    const TextInput in = text_area_input(field_id, rect, value, {.hint = options.hint});
    context.semantics(field_id, core::Role::TextField, shown(id), value);
    Edit edit;
    edit.changed = in.changed;
    edit.committed = in.deactivated && in.edited && value != original;
    if (!edit.committed) value = original;
    detail::track_field(kit().item.id, "\"" + original + "\"", "\"" + value + "\"", in.activated, edit);
    return edit;
}

bool text_entry(const char* id, std::string& value, TextEntryOptions options) {
    if (options.focus) ui().set_focus(ui().id(id));
    if (options.prompt == nullptr) return field(id, value, {.hint = options.hint}).entered;

    // The prompt sits where the text would begin, the text after it.
    const float prompt_width = measure(options.prompt).x;
    const bool entered = field(id, value, {.hint = options.hint, .lead = prompt_width + theme::GAP_XS}).entered;
    const core::Rect rect = kit().item.rect;
    const float text_y = rect.min.y + std::floor((rect.height() - text_style().size) * 0.5f);
    draw_text({rect.min.x + kit().style.frame_padding.x, text_y}, theme::text_secondary(), options.prompt);
    return entered;
}

Edit TextField::draw(const char* id, std::string_view value) {
    text_.assign(value);
    const Edit edit = text_field(id, text_);
    editing_ = kit().item.focused;
    return edit;
}

} // namespace ftk::ui
