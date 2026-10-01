#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ftk::platform {

/// Cross-platform file/directory watcher.
///
/// Watches one or more paths for modifications. Call poll() each frame
/// to check for changes — the callback fires synchronously during poll().
///
/// Linux: inotify. Windows: ReadDirectoryChangesW. macOS: FSEvents (future).
class FileWatcher {
public:
    /// Called when a watched file or a file inside a watched directory changes.
    /// `path` is the full path of the changed file.
    using Callback = std::function<void(const std::filesystem::path& path)>;

    FileWatcher();
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;
    FileWatcher(FileWatcher&&) noexcept;
    FileWatcher& operator=(FileWatcher&&) noexcept;

    /// Watch a file or directory for changes.
    /// For directories, events fire for files modified within them.
    /// Returns false if the watch could not be established.
    bool watch(const std::filesystem::path& path, Callback callback);

    /// Remove all watches.
    void clear();

    /// Poll for pending events. Returns true if any callback fired.
    [[nodiscard]] bool poll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ftk::platform
