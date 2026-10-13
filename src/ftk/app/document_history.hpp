#pragma once

#include "ftk/app/command_history.hpp"
#include "ftk/base/result.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ftk {

/// Undo for an editor whose edits change its data directly rather than
/// through commands. It keeps the document (the asset written as text) as
/// the last undo step left it; when an edit finishes and the document
/// differs, it records one step from the old text to the new, and undoing
/// puts the old text back through `restore`. Steps the editor records
/// itself are followed, not recorded twice.
class DocumentHistory {
public:
    using Document = std::function<std::string()>;
    using Restore = std::function<Result<>(const std::string&)>;

    DocumentHistory(CommandHistory& history, Document document, Restore restore);

    /// The document has just been opened or reloaded: what it holds now is
    /// where undo starts from.
    void opened();

    /// Called once a frame. `editing` while a field is being dragged or
    /// typed into; `input_finished` when a click or key press this frame
    /// could have finished an edit. `what` names the step.
    void frame(bool editing, bool input_finished, const std::string& what);

    /// Puts `text` in as one undo step named `what` (Revert).
    void change_to(const std::string& text, const std::string& what);

private:
    friend class DocumentEdit;
    void apply(const std::string& text);

    CommandHistory& history_;
    Document document_;
    Restore restore_;
    std::string recorded_;
    int recorded_at_{-1};
    bool was_editing_{false};
};

/// What an editor holding a document does when its file is written on disk.
enum class FileWritten : uint8_t {
    /// The file says what was last saved: the editor's own save, or a write
    /// that changes nothing.
    nothing,
    /// Nothing is unsaved here, or the file says what is held: the file's
    /// document is taken in, and is what was last saved.
    take,
    /// There are unsaved edits here: whether to keep them is asked.
    ask,
};

/// What to do with `on_disk`, the file's document as the editor would write
/// it, given `saved`, the document as last opened or saved, and `held`, the
/// one the editor holds now.
[[nodiscard]] FileWritten on_file_written(std::string_view on_disk, std::string_view saved, std::string_view held);

} // namespace ftk
