#include "ftk/platform/tree_watcher.hpp"
#include "ftk/base/log.hpp"

#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <string>
#include <unordered_map>

namespace ftk::platform {

namespace {

// What every folder is watched for: files made, written, gone and moved,
// and the folder itself going.
constexpr uint32_t WATCHED = IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_ONLYDIR;

// Whether `path` is `folder` or inside it.
bool within(const std::filesystem::path& path, const std::filesystem::path& folder) {
    const std::string p = path.generic_string();
    const std::string f = folder.generic_string();
    return p == f || (p.size() > f.size() && p.starts_with(f) && p[f.size()] == '/');
}

} // namespace

struct TreeWatcher::Impl {
    int fd{-1};
    std::filesystem::path root;
    SkipFolder skip;
    // Each watched folder by its watch, and the watch by the folder.
    std::unordered_map<int, std::filesystem::path> folders;
    std::unordered_map<std::string, int> watches;
    // The first half of a move, waiting for its second by the cookie the two
    // share; one left waiting at the end of a poll moved out of the tree.
    struct MoveOut {
        std::filesystem::path from;
        bool folder{false};
    };
    std::unordered_map<uint32_t, MoveOut> moves;
    bool warned_full{false};

    ~Impl() { stop(); }

    void stop() {
        if (fd >= 0) close(fd);
        fd = -1;
        folders.clear();
        watches.clear();
        moves.clear();
    }

    [[nodiscard]] bool skipped(const std::filesystem::path& folder) const {
        return skip && skip(folder.filename().string());
    }

    // Watches `folder` and the folders under it; with `report`, every file
    // and folder already in it is added to `events`, as one that appears
    // with contents has them before its watch can see them made.
    void watch_folder(const std::filesystem::path& folder, bool report, std::vector<TreeEvent>& events) {
        const int wd = inotify_add_watch(fd, folder.c_str(), WATCHED);
        if (wd < 0) {
            if (errno == ENOSPC && !warned_full) {
                warned_full = true;
                FTK_CORE_WARN("TreeWatcher: the system's limit of inotify watches is reached; {} and others "
                              "are not watched (raise fs.inotify.max_user_watches)",
                              folder.string());
            }
            return;
        }
        folders[wd] = folder;
        watches[folder.generic_string()] = wd;

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            if (entry.is_directory(ec)) {
                if (skipped(entry.path())) continue;
                if (report) events.push_back({.kind = TreeEvent::Kind::added, .path = entry.path(), .folder = true});
                watch_folder(entry.path(), report, events);
            } else if (report) {
                events.push_back({.kind = TreeEvent::Kind::added, .path = entry.path()});
            }
        }
    }

    // A folder moved out of the tree or gone: its watches and those under it
    // stop, as the kernel would otherwise keep following it wherever it went.
    void unwatch(const std::filesystem::path& folder) {
        for (auto it = folders.begin(); it != folders.end();) {
            if (within(it->second, folder)) {
                inotify_rm_watch(fd, it->first);
                watches.erase(it->second.generic_string());
                it = folders.erase(it);
            } else {
                ++it;
            }
        }
    }

    // A folder renamed within the tree: its watches follow it, so only the
    // names they are known by change.
    void rename(const std::filesystem::path& from, const std::filesystem::path& to) {
        const std::string old_prefix = from.generic_string();
        for (auto& [wd, folder] : folders) {
            if (!within(folder, from)) continue;
            watches.erase(folder.generic_string());
            folder = std::filesystem::path(to.generic_string() + folder.generic_string().substr(old_prefix.size()));
            watches[folder.generic_string()] = wd;
        }
    }

    bool watch(const std::filesystem::path& folder, SkipFolder skip_folder) {
        stop();
        fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd < 0) {
            FTK_CORE_WARN("TreeWatcher: inotify could not be started");
            return false;
        }
        root = folder;
        skip = std::move(skip_folder);
        std::vector<TreeEvent> none;
        watch_folder(root, false, none);
        if (folders.empty()) {
            stop();
            return false;
        }
        return true;
    }

    std::vector<TreeEvent> poll() {
        std::vector<TreeEvent> events;
        if (fd < 0) return events;

        alignas(inotify_event) std::array<char, 16384> buffer{};
        for (ssize_t length = read(fd, buffer.data(), buffer.size()); length > 0;
             length = read(fd, buffer.data(), buffer.size())) {
            for (const char* at = buffer.data(); at < buffer.data() + length;) {
                const auto* event = reinterpret_cast<const inotify_event*>(at);
                at += sizeof(inotify_event) + event->len;
                take(*event, events);
            }
        }
        // A move whose second half never came left the tree.
        for (auto& [cookie, move] : moves) {
            if (move.folder) unwatch(move.from);
            events.push_back({.kind = TreeEvent::Kind::removed, .path = std::move(move.from), .folder = move.folder});
        }
        moves.clear();
        return tidy_tree_events(std::move(events));
    }

    void take(const inotify_event& event, std::vector<TreeEvent>& events) {
        using Kind = TreeEvent::Kind;
        if ((event.mask & IN_Q_OVERFLOW) != 0) {
            events.push_back({.kind = Kind::lost, .path = root});
            return;
        }
        if ((event.mask & IN_IGNORED) != 0) {
            const auto found = folders.find(event.wd);
            if (found != folders.end()) {
                watches.erase(found->second.generic_string());
                folders.erase(found);
            }
            return;
        }
        const auto found = folders.find(event.wd);
        if (found == folders.end() || event.len == 0) return;
        const std::filesystem::path path = found->second / event.name;
        const bool folder = (event.mask & IN_ISDIR) != 0;
        if (folder && skipped(path)) return;

        if ((event.mask & IN_CREATE) != 0) {
            events.push_back({.kind = Kind::added, .path = path, .folder = folder});
            if (folder) watch_folder(path, true, events);
        } else if ((event.mask & IN_CLOSE_WRITE) != 0) {
            events.push_back({.kind = Kind::changed, .path = path});
        } else if ((event.mask & IN_DELETE) != 0) {
            events.push_back({.kind = Kind::removed, .path = path, .folder = folder});
        } else if ((event.mask & IN_MOVED_FROM) != 0) {
            moves[event.cookie] = {.from = path, .folder = folder};
        } else if ((event.mask & IN_MOVED_TO) != 0) {
            const auto move = moves.find(event.cookie);
            if (move != moves.end()) {
                if (folder) rename(move->second.from, path);
                events.push_back({.kind = Kind::renamed, .path = path, .from = move->second.from, .folder = folder});
                moves.erase(move);
            } else {
                events.push_back({.kind = Kind::added, .path = path, .folder = folder});
                if (folder) watch_folder(path, true, events);
            }
        }
    }
};

TreeWatcher::TreeWatcher() : impl_{std::make_unique<Impl>()} {}
TreeWatcher::~TreeWatcher() = default;
TreeWatcher::TreeWatcher(TreeWatcher&&) noexcept = default;
TreeWatcher& TreeWatcher::operator=(TreeWatcher&&) noexcept = default;

bool TreeWatcher::watch(const std::filesystem::path& root, SkipFolder skip) {
    return impl_->watch(root, std::move(skip));
}

void TreeWatcher::stop() { impl_->stop(); }

std::vector<TreeEvent> TreeWatcher::poll() { return impl_->poll(); }

} // namespace ftk::platform
