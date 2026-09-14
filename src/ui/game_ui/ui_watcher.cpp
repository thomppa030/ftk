#ifndef NDEBUG

#include "ui/game_ui/ui_watcher.hpp"
#include "core/log.hpp"

namespace fjell {

UiWatcher::UiWatcher(const std::vector<std::string>& paths, ChangeCallback callback)
    : callback_{std::move(callback)} {
    size_t watched = 0;
    for (const auto& path : paths) {
        if (path.empty()) continue;
        bool ok = watcher_.watch(path, [this](const std::filesystem::path& changed) {
            auto key = changed.lexically_normal().string();
            if (expected_.contains(key)) {
                expected_hit_.insert(key);
                return;
            }
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

void UiWatcher::expect_change(const std::string& path) {
    // Same normalization as the key the watcher reports, so "a/./b.fjss"
    // and "a/b.fjss" name the same expectation.
    expected_.insert(std::filesystem::path(path).lexically_normal().string());
}

bool UiWatcher::poll() {
    changed_ = false;
    (void)watcher_.poll();
    // A write raises several events (modify, close) in the same poll; all of
    // them were swallowed above, and the expectation ends with that poll.
    for (const auto& path : expected_hit_) expected_.erase(path);
    expected_hit_.clear();

    if (changed_ && callback_) {
        FJELL_CORE_INFO("UiWatcher: source file changed, reload requested");
        callback_();
        return true;
    }
    return false;
}

} // namespace fjell

#endif // NDEBUG
