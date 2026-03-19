#pragma once

#ifndef NDEBUG

#include <functional>
#include <string>

namespace fjell {

/// Watches .fjui/.fjss files for changes using inotify (debug only).
/// Triggers a callback when a watched file is modified.
class UiWatcher {
public:
    using ReloadCallback = std::function<void(const std::string& fjui_path,
                                              const std::string& fjss_path)>;

    UiWatcher(const std::string& fjui_path, const std::string& fjss_path,
              ReloadCallback callback);
    ~UiWatcher();

    UiWatcher(const UiWatcher&) = delete;
    UiWatcher& operator=(const UiWatcher&) = delete;
    UiWatcher(UiWatcher&&) = delete;
    UiWatcher& operator=(UiWatcher&&) = delete;

    /// Poll for file changes. Returns true if a reload was triggered.
    [[nodiscard]] bool poll();

private:
    std::string fjui_path_;
    std::string fjss_path_;
    ReloadCallback callback_;
    int inotify_fd_{-1};
    int fjui_wd_{-1};
    int fjss_wd_{-1};
};

} // namespace fjell

#endif // NDEBUG
