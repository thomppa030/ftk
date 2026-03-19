#ifndef NDEBUG

#include "ui/game_ui/ui_watcher.hpp"
#include "core/log.hpp"

namespace fjell {

UiWatcher::UiWatcher(const std::string& fjui_path, const std::string& fjss_path,
                     ReloadCallback callback)
    : fjui_path_{fjui_path}
    , fjss_path_{fjss_path}
    , callback_{std::move(callback)} {
    watcher_.watch(fjui_path_, [this](const std::filesystem::path&) {
        changed_ = true;
    });

    if (!fjss_path_.empty()) {
        watcher_.watch(fjss_path_, [this](const std::filesystem::path&) {
            changed_ = true;
        });
    }

    FJELL_GFX_DEBUG("UiWatcher: watching {} and {}", fjui_path_, fjss_path_);
}

bool UiWatcher::poll() {
    changed_ = false;
    (void)watcher_.poll();

    if (changed_ && callback_) {
        FJELL_CORE_INFO("UiWatcher: file changed, reloading UI");
        callback_(fjui_path_, fjss_path_);
        return true;
    }
    return false;
}

} // namespace fjell

#endif // NDEBUG
