#include "ftk/ui/kit/other_window.hpp"

#include "ftk/ui/kit/core/kit.hpp"

namespace ftk::ui {

using namespace corekit;

OtherWindow::OtherWindow(const char* id, const OtherWindowSpec& spec) : root_{other_window_root(id)} {
    core::Context& context = ui();
    Kit& k = kit();
    const auto asked = k.other_windows.find(root_);
    // Closed by its user: told once, and forgotten, so drawing it again
    // opens it anew.
    if (asked != k.other_windows.end() && context.root_closed(root_)) {
        k.other_windows.erase(asked);
        k.bars.erase(root_);
        closed_ = true;
        return;
    }
    // Asked for once, and again when its title changes; the program leaves
    // its size and place as its user made them.
    if (asked == k.other_windows.end() || asked->second != spec.title) {
        context.open_root({.root = root_,
                           .kind = core::RootKind::Window,
                           .size = spec.size,
                           .near = context.root(),
                           .at = spec.at,
                           .title = spec.title});
        k.other_windows[root_] = spec.title;
    }
    close_asked_ = context.take_close_ask(root_);
    open_ = context.begin_other_root(root_);
}

OtherWindow::~OtherWindow() {
    if (open_) ui().end_other_root();
}

void OtherWindow::raise() {
    ui().raise_root(root_);
}

void close_other_window(const char* id) {
    Kit& k = kit();
    const core::RootId root = other_window_root(id);
    if (k.other_windows.erase(root) == 0) return;
    k.bars.erase(root);
    ui().close_root(root);
}

core::RootId other_window_root(const char* id) {
    return core::hash_label("#OTHER_WINDOW", core::hash_label(id, 0));
}

} // namespace ftk::ui
