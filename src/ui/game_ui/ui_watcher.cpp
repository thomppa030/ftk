#ifndef NDEBUG

#include "ui/game_ui/ui_watcher.hpp"
#include "core/log.hpp"

namespace fjell {

UiWatcher::UiWatcher(const std::vector<std::string>& paths, ChangeCallback callback)
    : callback_{std::move(callback)} {
    size_t watched = 0;
    for (const auto& path : paths) {
        if (path.empty()) continue;
        bool ok = watcher_.watch(path, [this](const std::filesystem::path&) {
            changed_ = true;
        });
        if (ok) {
            ++watched;
        } else {
            FJELL_GFX_DEBUG("UiWatcher: cannot watch {}", path);
        }
    }
    FJELL_GFX_DEBUG("UiWatcher: watching {} files", watched);
}

bool UiWatcher::poll() {
    changed_ = false;
    (void)watcher_.poll();

    if (changed_ && callback_) {
        FJELL_CORE_INFO("UiWatcher: source file changed, reload requested");
        callback_();
        return true;
    }
    return false;
}

} // namespace fjell

#endif // NDEBUG
