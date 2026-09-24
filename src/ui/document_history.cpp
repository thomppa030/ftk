#include "ui/document_history.hpp"

#include "core/log.hpp"

#include <memory>
#include <utility>

namespace fjell {

/// One edit made directly to a document, undone and redone by putting the
/// text before or after it back.
class DocumentEdit final : public Command {
public:
    DocumentEdit(DocumentHistory& owner, std::string before, std::string after, std::string description)
        : owner_{owner}, before_{std::move(before)}, after_{std::move(after)},
          description_{std::move(description)} {}

    void execute() override { owner_.apply(after_); }
    void undo() override { owner_.apply(before_); }
    [[nodiscard]] std::string description() const override { return description_; }

private:
    DocumentHistory& owner_;
    std::string before_;
    std::string after_;
    std::string description_;
};

DocumentHistory::DocumentHistory(CommandHistory& history, Document document, Restore restore)
    : history_{history}, document_{std::move(document)}, restore_{std::move(restore)} {}

void DocumentHistory::opened() {
    recorded_ = document_();
    recorded_at_ = history_.current_index();
}

void DocumentHistory::frame(bool editing, bool input_finished, const std::string& what) {
    if (editing) {
        was_editing_ = true;
        return;
    }
    const bool may_have_finished = was_editing_ || input_finished;
    was_editing_ = false;
    if (!may_have_finished) return;

    std::string now = document_();
    // A step the editor recorded itself, or an undo: follow it.
    if (history_.current_index() != recorded_at_) {
        recorded_ = std::move(now);
        recorded_at_ = history_.current_index();
        return;
    }
    if (now.empty() || now == recorded_) return;

    history_.record(std::make_unique<DocumentEdit>(*this, recorded_, now, what));
    recorded_ = std::move(now);
    recorded_at_ = history_.current_index();
}

void DocumentHistory::change_to(const std::string& text, const std::string& what) {
    std::string now = document_();
    if (now == text) return;
    history_.execute(std::make_unique<DocumentEdit>(*this, std::move(now), text, what));
    recorded_at_ = history_.current_index();
}

void DocumentHistory::apply(const std::string& text) {
    if (auto restored = restore_(text); !restored) {
        FJELL_CORE_ERROR("Couldn't put the document back: {}", restored.error());
    }
    // The history's position moves once this returns; frame() follows it.
    recorded_ = document_();
}

} // namespace fjell
