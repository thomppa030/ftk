#pragma once

#ifndef NDEBUG

#include "platform/file_watcher.hpp"

#include <functional>
#include <string>

namespace fjell {

/// Hot-reload watcher for `.fjgraph` files (debug builds only).
///
/// V1 scope: re-parses on file change and invokes the callback. The
/// callback is responsible for diffing + applying changes — the watcher
/// only owns the inotify watch and the change flag. Pass-shape changes
/// (adds/removes/param changes) require a restart in v1; only `enabled`
/// flags are honored live.
class FjGraphWatcher {
public:
    using ReloadCallback = std::function<void(const std::string& graph_path)>;

    FjGraphWatcher(std::string graph_path, ReloadCallback callback);
    ~FjGraphWatcher() = default;

    FjGraphWatcher(const FjGraphWatcher&) = delete;
    FjGraphWatcher& operator=(const FjGraphWatcher&) = delete;
    FjGraphWatcher(FjGraphWatcher&&) = delete;
    FjGraphWatcher& operator=(FjGraphWatcher&&) = delete;

    /// Drain pending inotify events. Calls the reload callback once if
    /// any change was seen since the last poll. Returns true when the
    /// callback fired.
    bool poll();

private:
    std::string graph_path_;
    ReloadCallback callback_;
    platform::FileWatcher watcher_;
    bool changed_{false};
};

} // namespace fjell

#endif // NDEBUG
