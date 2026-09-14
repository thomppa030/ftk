#pragma once

#ifndef NDEBUG

#include "platform/file_watcher.hpp"

#include <functional>
#include <string>
#include <vector>

namespace fjell {

/// Watches the files a loaded UI was built from (debug only) — root markup,
/// stylesheet, and every widget pulled in by tag — and reports when any of
/// them changes on disk. The owner decides how to rebuild; the watcher only
/// knows that what is loaded no longer matches the files.
class UiWatcher {
public:
    using ChangeCallback = std::function<void()>;

    /// Empty paths are skipped; a path that cannot be watched is logged and
    /// ignored so one missing optional file never disables the rest.
    UiWatcher(const std::vector<std::string>& paths, ChangeCallback callback);
    ~UiWatcher() = default;

    UiWatcher(const UiWatcher&) = delete;
    UiWatcher& operator=(const UiWatcher&) = delete;
    UiWatcher(UiWatcher&&) = delete;
    UiWatcher& operator=(UiWatcher&&) = delete;

    /// Checks for changes since the last poll. Invokes the callback and
    /// returns true if any watched file was modified.
    [[nodiscard]] bool poll();

private:
    ChangeCallback callback_;
    platform::FileWatcher watcher_;
    bool changed_{false};
};

} // namespace fjell

#endif // NDEBUG
