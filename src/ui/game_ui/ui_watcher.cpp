#ifndef NDEBUG

#include "ui/game_ui/ui_watcher.hpp"
#include "core/log.hpp"

#include <sys/inotify.h>
#include <unistd.h>

#include <cstring>

namespace fjell {

UiWatcher::UiWatcher(const std::string& fjui_path, const std::string& fjss_path,
                     ReloadCallback callback)
    : fjui_path_{fjui_path}
    , fjss_path_{fjss_path}
    , callback_{std::move(callback)} {
    inotify_fd_ = inotify_init1(IN_NONBLOCK);
    if (inotify_fd_ < 0) {
        FJELL_CORE_WARN("UiWatcher: failed to init inotify");
        return;
    }

    fjui_wd_ = inotify_add_watch(inotify_fd_, fjui_path_.c_str(),
                                  IN_MODIFY | IN_CLOSE_WRITE);
    if (fjui_wd_ < 0) {
        FJELL_CORE_WARN("UiWatcher: failed to watch {}", fjui_path_);
    }

    if (!fjss_path_.empty()) {
        fjss_wd_ = inotify_add_watch(inotify_fd_, fjss_path_.c_str(),
                                      IN_MODIFY | IN_CLOSE_WRITE);
        if (fjss_wd_ < 0) {
            FJELL_CORE_WARN("UiWatcher: failed to watch {}", fjss_path_);
        }
    }

    FJELL_GFX_DEBUG("UiWatcher: watching {} and {}", fjui_path_, fjss_path_);
}

UiWatcher::~UiWatcher() {
    if (fjui_wd_ >= 0) inotify_rm_watch(inotify_fd_, fjui_wd_);
    if (fjss_wd_ >= 0) inotify_rm_watch(inotify_fd_, fjss_wd_);
    if (inotify_fd_ >= 0) close(inotify_fd_);
}

bool UiWatcher::poll() {
    if (inotify_fd_ < 0) return false;

    alignas(struct inotify_event) char buf[4096];
    auto len = read(inotify_fd_, buf, sizeof(buf));
    if (len <= 0) return false;

    // Drain all events, then reload once
    bool changed = false;
    const char* ptr = buf;
    while (ptr < buf + len) {
        const auto* event = reinterpret_cast<const struct inotify_event*>(ptr);
        if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
            changed = true;
        }
        ptr += sizeof(struct inotify_event) + event->len;
    }

    if (changed && callback_) {
        FJELL_CORE_INFO("UiWatcher: file changed, reloading UI");
        callback_(fjui_path_, fjss_path_);
        return true;
    }
    return false;
}

} // namespace fjell

#endif // NDEBUG
