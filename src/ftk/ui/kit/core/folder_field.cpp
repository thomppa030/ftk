#include "ftk/ui/kit/folder_field.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text_field.hpp"

#include <algorithm>

namespace ftk::ui {

using namespace corekit;

FolderField folder_field(const char* id, std::string& value, FolderFieldOptions options) {
    // The width asked for is the pair's: the button and the space before it
    // come out of the text's share.
    const float whole = item_width();
    next_width(std::max(whole - frame_height() - item_spacing().x, 1.0f));

    FolderField result;
    if (options.live) {
        const std::string before = value;
        result.edit.committed = text_entry(id, value, {.hint = options.hint});
        result.edit.changed = value != before;
    } else {
        result.edit = text_field(id, value, {.hint = options.hint});
    }
    same_line();
    // Under the field's own id, so two folder fields keep their buttons apart.
    const IdScope scope(id);
    result.browse = icon_button("##browse", icon::folder, "Choose the folder");
    return result;
}

} // namespace ftk::ui
