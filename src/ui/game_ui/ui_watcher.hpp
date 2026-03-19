#pragma once

#ifndef NDEBUG

#include "platform/file_watcher.hpp"

#include <functional>
#include <string>

namespace fjell {

/// Watches .fjui/.fjss files for changes (debug only).
/// Triggers a callback when a watched file is modified.
class UiWatcher {
public:
    using ReloadCallback = std::function<void(const std::string& fjui_path,
                                              const std::string& fjss_path)>;

    UiWatcher(const std::string& fjui_path, const std::string& fjss_path,
              ReloadCallback callback);
    ~UiWatcher() = default;

    UiWatcher(const UiWatcher&) = delete;
    UiWatcher& operator=(const UiWatcher&) = delete;
    UiWatcher(UiWatcher&&) = delete;
    UiWatcher& operator=(UiWatcher&&) = delete;

    [[nodiscard]] bool poll();

private:
    std::string fjui_path_;
    std::string fjss_path_;
    ReloadCallback callback_;
    platform::FileWatcher watcher_;
    bool changed_{false};
};

} // namespace fjell

#endif // NDEBUG
