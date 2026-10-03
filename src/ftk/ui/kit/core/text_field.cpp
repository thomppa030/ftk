#include "ftk/ui/kit/text_field.hpp"

#include "ftk/ui/kit/core/text_input.hpp"
#include "ftk/ui/kit/edit_record.hpp"
#include "ftk/ui/kit/layout.hpp"

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

bool text_entry(const char* id, std::string& value, TextEntryOptions options) {
    if (options.focus) ui().set_focus(ui().id(id));
    return field(id, value, {.hint = options.hint}).entered;
}

Edit TextField::draw(const char* id, std::string_view value) {
    text_.assign(value);
    const Edit edit = text_field(id, text_);
    editing_ = kit().item.focused;
    return edit;
}

} // namespace ftk::ui
