// What each widget is, recorded per frame (decision 10 of the UI core
// plan). A frame's records are read once it has ended: the harness finds
// widgets by them between frames.

#include "ftk/ui/core/context.hpp"

#include <algorithm>

namespace ftk::ui::core {

void Context::semantics(Id id, Role role, std::string_view label, std::string_view value, SemanticState state) {
    semantics(id, role, label, value, state, layout_.last_item());
}

void Context::semantics(Id id, Role role, std::string_view label, std::string_view value, SemanticState state,
                        Rect rect) {
    // Not shown this frame, so not there to find.
    if (hidden()) return;
    label = shown_label(label);
    Recorded recorded;
    recorded.node = {.id = id, .role = role, .state = state, .rect = rect, .root = root_ != nullptr ? root_->id : 0};
    recorded.node.state.disabled = state.disabled || disabled();
    recorded.node.state.focused = focused_ == id;
    recorded.label = semantics_text_.size();
    recorded.label_size = label.size();
    semantics_text_ += label;
    recorded.value = semantics_text_.size();
    recorded.value_size = value.size();
    semantics_text_ += value;
    semantics_.push_back(recorded);
}

const SemanticNode* Context::find(std::string_view label, Role role) const {
    const auto found = std::ranges::find_if(semantics_done_, [&](const SemanticNode& node) {
        return node.label == label && (role == Role::None || node.role == role);
    });
    return found != semantics_done_.end() ? &*found : nullptr;
}

void Context::finish_semantics() {
    // The frame's records become the ones read, their text with them; the
    // buffers are kept for the next frame.
    std::swap(semantics_done_text_, semantics_text_);
    semantics_text_.clear();
    const std::string_view text{semantics_done_text_};
    semantics_done_.clear();
    for (const Recorded& recorded : semantics_) {
        SemanticNode node = recorded.node;
        node.label = text.substr(recorded.label, recorded.label_size);
        node.value = text.substr(recorded.value, recorded.value_size);
        semantics_done_.push_back(node);
    }
    semantics_.clear();
}

} // namespace ftk::ui::core
