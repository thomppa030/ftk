#include "ftk/ui/kit/edit_record.hpp"

#include <cstring>
#include <unordered_map>
#include <utility>

namespace ftk::ui {

namespace {

struct Pending {
    std::string scope{};
    EditRecord record{};
    int frame{-1};
    // Finished edits in the scope since the last take. More than one is an
    // edit of several values and gets no record.
    int count{0};
};

std::string g_scope;
std::string g_label;
Pending g_pending;
// The text each field read when its edit under way started.
std::unordered_map<ImGuiID, std::string> g_started;

} // namespace

void edit_scope(std::string scope) {
    g_scope = std::move(scope);
    g_label.clear();
}

std::optional<EditRecord> take_edit_record(const std::string& scope) {
    if (g_pending.scope != scope || g_pending.count != 1 || g_pending.record.label.empty()) {
        if (g_pending.scope == scope) g_pending = {};
        return std::nullopt;
    }
    EditRecord record = std::move(g_pending.record);
    g_pending = {};
    return record;
}

namespace detail {

void set_edit_label(const char* label) {
    g_label = label != nullptr ? label : "";
    // "Mass##rigidbody" is shown as "Mass".
    if (auto hash = g_label.find("##"); hash != std::string::npos) g_label.resize(hash);
}

void track_field(ImGuiID id, const std::string& before, const std::string& after, bool started,
                 const Edit& edit) {
    if (started) g_started[id] = before;
    if (!edit.committed) return;
    auto it = g_started.find(id);
    std::string from = it != g_started.end() ? std::move(it->second) : before;
    if (it != g_started.end()) g_started.erase(it);
    if (g_scope.empty() || from == after) return;

    const int frame = ImGui::GetFrameCount();
    if (g_pending.scope != g_scope) g_pending = {.scope = g_scope};
    // An outer field replaces the inner one it drew on the same frame.
    if (g_pending.frame != frame || g_pending.count == 0) ++g_pending.count;
    g_pending.frame = frame;
    g_pending.record = {.label = g_label, .before = std::move(from), .after = after};
}

std::string with_unit(const char* number, const char* unit_symbol) {
    std::string text = number;
    if (unit_symbol == nullptr || *unit_symbol == '\0') return text;
    // Degrees and percent sit on the number; other units follow a space.
    if (std::strcmp(unit_symbol, "%") != 0 && std::strcmp(unit_symbol, "\xc2\xb0") != 0) text += ' ';
    return text + unit_symbol;
}

} // namespace detail

} // namespace ftk::ui
