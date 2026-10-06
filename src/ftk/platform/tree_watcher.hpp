#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace ftk::platform {

/// Something that happened under a watched tree.
struct TreeEvent {
    enum class Kind : uint8_t {
        /// A file or folder is there that was not: made, copied or moved in,
        /// or a file replaced whole by one renamed over it.
        added,
        /// A file's content was written.
        changed,
        /// A file or folder is gone: deleted, or moved out of the tree. A
        /// folder's files are not reported one by one, and on some systems
        /// a removed folder is not known to have been one: a reader takes a
        /// removed path for the file or for everything under it.
        removed,
        /// A file or folder moved within the tree, from `from` to `path`.
        renamed,
        /// The system dropped events: what is under the root is to be read
        /// again. `path` is the root.
        lost,
    };

    Kind kind{Kind::changed};
    std::filesystem::path path{};
    /// Where a renamed file or folder was.
    std::filesystem::path from{};
    /// The event is about a folder.
    bool folder{false};

    bool operator==(const TreeEvent&) const = default;
};

/// The events of one poll as a reader needs them, in order: a file added
/// then written is added; writes in a row are one change; a file added and
/// removed again is nothing; one added and then renamed is added where it
/// went; one removed and added again is changed; one renamed and then
/// removed is removed from where it was.
[[nodiscard]] std::vector<TreeEvent> tidy_tree_events(std::vector<TreeEvent> events);

/// Watches a folder and everything under it, telling what happened to each
/// file and folder since the last poll: added, changed, removed, renamed.
/// Folders `skip` names are left unwatched with all under them. Call poll()
/// from one thread; nothing happens between polls but the system's queueing.
///
/// Linux: inotify, a watch per folder, a folder that appears watched as it
/// comes and the files already in it reported added. Windows:
/// ReadDirectoryChangesW over the whole subtree, which reports a little
/// after the change, not at once.
///
/// @code
/// platform::TreeWatcher watcher;
/// watcher.watch(project_root, [](std::string_view name) { return name == "build"; });
/// for (const auto& event : watcher.poll()) { ... }   // once a frame
/// @endcode
class TreeWatcher {
public:
    /// Whether a folder of this name is left unwatched, with all under it.
    using SkipFolder = std::function<bool(std::string_view name)>;

    TreeWatcher();
    ~TreeWatcher();

    TreeWatcher(const TreeWatcher&) = delete;
    TreeWatcher& operator=(const TreeWatcher&) = delete;
    TreeWatcher(TreeWatcher&&) noexcept;
    TreeWatcher& operator=(TreeWatcher&&) noexcept;

    /// Watches `root` and everything under it, in place of what was watched.
    /// @return false when `root` cannot be watched
    bool watch(const std::filesystem::path& root, SkipFolder skip = {});

    /// Stops watching.
    void stop();

    /// What happened since the last poll, tidied by tidy_tree_events(). Never
    /// waits.
    [[nodiscard]] std::vector<TreeEvent> poll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ftk::platform
