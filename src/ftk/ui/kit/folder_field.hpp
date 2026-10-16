#pragma once

#include "ftk/ui/kit/edit.hpp"

#include <string>

namespace ftk::ui {

struct FolderFieldOptions {
    /// Shown dimmed while the field is empty: what an empty field stands
    /// for ("Beside each source").
    const char* hint{nullptr};
    /// The text is written into `value` as it is typed, as text_entry()'s
    /// is, for a folder a button waits on; `committed` is then Enter.
    /// Otherwise it commits as text_field() does.
    bool live{false};
};

/// What a folder field did this frame.
struct FolderField {
    /// The text's edit, as a text field reports it.
    Edit edit;
    /// The folder button was pressed: the caller opens the folder browser
    /// at the field's folder and puts the one chosen into `value`.
    bool browse{false};
};

/// A field for a folder (the kit spec's Folder field): a text field with a
/// folder button at its end, its tooltip "Choose the folder". Field and
/// button together are as wide as the next item is to be.
///
///     const ui::FolderField output = ui::folder_field("##output", folder, {.hint = "Beside each source"});
///     if (output.edit.committed) set_output(folder);
///     if (output.browse) browser.open("Output folder", FileBrowser::Mode::select_directory, "Choose", {.start = folder});
FolderField folder_field(const char* id, std::string& value, FolderFieldOptions options = {});

} // namespace ftk::ui
