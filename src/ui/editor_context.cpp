#include "ui/editor_context.hpp"
#include "ui/console.hpp"
#include "ui/kit/edit_record.hpp"
#include "core/log.hpp"

namespace fjell {

void EditorContext::draw_shared_panels(float dt) {
    auto stats_title = ctx_title("Stats");
    auto history_title = ctx_title("History");
    auto console_title = ctx_title("Console");

    stats_panel_.draw(dt, stats_title.c_str());
    history_panel_.draw(history_title.c_str());
    if (auto& sink = log::console_sink(); sink) {
        sink->draw(console_title.c_str());
    }
}

namespace {

// How long has_unsaved_changes() reuses its answer, in seconds.
constexpr double UNSAVED_RECHECK = 0.25;

// Whether this frame could have finished an edit made without holding an
// item: a click released, a key pressed (Delete, Enter), a drop.
bool input_finished_something() {
    for (int b = 0; b < ImGuiMouseButton_COUNT; ++b) {
        if (ImGui::IsMouseReleased(b)) return true;
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) return true;
    }
    return false;
}

} // namespace

void EditorContext::draw_frame(float dt) {
    const std::string scope = "context/" + std::to_string(context_index_);
    ui::edit_scope(scope);
    draw(dt);
    ui::edit_scope({});

    const bool editing = ImGui::IsAnyItemActive();
    const bool finished = !editing && input_finished_something();
    std::string what = std::string("Edit ") + name();
    if (finished || !editing) {
        if (auto record = ui::take_edit_record(scope)) {
            what = describe_change(record->label, record->before, record->after);
        }
    }
    document_history_.frame(editing, finished, what);
}

Result<> EditorContext::restore_document(const std::string& text) {
    auto restored = restore(text);
    unsaved_checked_at_ = -1.0;
    return restored;
}

Result<> EditorContext::save() {
    if (auto written = write(); !written) return written;
    saved_document_ = document();
    unsaved_ = false;
    unsaved_checked_at_ = ImGui::GetTime();
    return {};
}

bool EditorContext::has_unsaved_changes() const {
    const double now = ImGui::GetTime();
    if (unsaved_checked_at_ < 0.0 || now - unsaved_checked_at_ >= UNSAVED_RECHECK) {
        unsaved_ = document() != saved_document_;
        unsaved_checked_at_ = now;
    }
    return unsaved_;
}

void EditorContext::set_saved_document(std::string text) {
    saved_document_ = std::move(text);
    unsaved_checked_at_ = -1.0;
}

void EditorContext::document_opened() {
    saved_document_ = document();
    document_history_.opened();
    unsaved_ = false;
    unsaved_checked_at_ = -1.0;
}

} // namespace fjell
