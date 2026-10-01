#include "ftk/platform/file_watcher.hpp"
#include "ftk/base/log.hpp"

#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace ftk::platform {

struct FileWatcher::Impl {
    int inotify_fd{-1};

    /// One caller's interest in a directory: either every file in it
    /// (empty file_name) or a single file by name.
    struct Subscriber {
        std::filesystem::path file_name;
        Callback callback;
    };

    /// inotify hands out one watch descriptor per directory no matter how
    /// many times it is added, so all files watched in the same directory
    /// share a single entry and are told apart by name when events arrive.
    struct WatchEntry {
        std::filesystem::path dir_path;
        std::vector<Subscriber> subscribers;
    };

    // inotify watch descriptor → entry
    std::unordered_map<int, WatchEntry> watches;

    Impl() {
        inotify_fd = inotify_init1(IN_NONBLOCK);
        if (inotify_fd < 0) {
            FTK_CORE_WARN("FileWatcher: failed to init inotify");
        }
    }

    ~Impl() {
        clear();
        if (inotify_fd >= 0) {
            close(inotify_fd);
        }
    }

    void clear() {
        for (auto& [wd, entry] : watches) {
            if (inotify_fd >= 0) {
                inotify_rm_watch(inotify_fd, wd);
            }
        }
        watches.clear();
    }

    bool add_watch(const std::filesystem::path& path, Callback callback) {
        if (inotify_fd < 0) return false;

        // Files are watched through their directory: editors that save by
        // writing a temporary file and renaming it over the original would
        // silently detach a watch placed on the file's own inode.
        bool is_dir = std::filesystem::is_directory(path);
        std::filesystem::path watch_path = is_dir ? path : path.parent_path();
        std::filesystem::path file_name = is_dir ? "" : path.filename();

        int wd = inotify_add_watch(inotify_fd, watch_path.c_str(),
                                   IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO);
        if (wd < 0) {
            FTK_CORE_WARN("FileWatcher: failed to watch {}", path.string());
            return false;
        }

        auto& entry = watches[wd];
        entry.dir_path = watch_path;
        entry.subscribers.push_back({file_name, std::move(callback)});
        return true;
    }

    bool poll_events() {
        if (inotify_fd < 0) return false;

        pollfd pfd{};
        pfd.fd = inotify_fd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 0) <= 0) return false;

        alignas(inotify_event) std::array<char, 4096> buf{};
        auto len = read(inotify_fd, buf.data(), buf.size());
        if (len <= 0) return false;

        bool any_fired = false;
        const char* ptr = buf.data();
        while (ptr < buf.data() + len) {
            const auto* event = reinterpret_cast<const inotify_event*>(ptr);
            ptr += sizeof(inotify_event) + event->len;

            auto it = watches.find(event->wd);
            // Events without a name concern the directory itself, not a
            // file in it; no subscriber is interested in those.
            if (it == watches.end() || event->len == 0) continue;

            const auto& entry = it->second;
            std::filesystem::path changed_file{event->name};
            for (const auto& sub : entry.subscribers) {
                if (sub.file_name.empty() || changed_file == sub.file_name) {
                    sub.callback(entry.dir_path / changed_file);
                    any_fired = true;
                }
            }
        }

        return any_fired;
    }
};

FileWatcher::FileWatcher() : impl_{std::make_unique<Impl>()} {}
FileWatcher::~FileWatcher() = default;
FileWatcher::FileWatcher(FileWatcher&&) noexcept = default;
FileWatcher& FileWatcher::operator=(FileWatcher&&) noexcept = default;

bool FileWatcher::watch(const std::filesystem::path& path, Callback callback) {
    return impl_->add_watch(path, std::move(callback));
}

void FileWatcher::clear() {
    impl_->clear();
}

bool FileWatcher::poll() {
    return impl_->poll_events();
}

} // namespace ftk::platform
