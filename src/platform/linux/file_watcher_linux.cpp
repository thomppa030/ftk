#include "platform/file_watcher.hpp"
#include "core/log.hpp"

#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <unordered_map>

namespace fjell::platform {

struct FileWatcher::Impl {
    int inotify_fd{-1};

    struct WatchEntry {
        std::filesystem::path base_path; // watched dir or file's parent
        std::filesystem::path file_name; // empty for directory watches
        Callback callback;
    };

    // inotify watch descriptor → entry
    std::unordered_map<int, WatchEntry> watches;

    Impl() {
        inotify_fd = inotify_init1(IN_NONBLOCK);
        if (inotify_fd < 0) {
            FJELL_CORE_WARN("FileWatcher: failed to init inotify");
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

        bool is_dir = std::filesystem::is_directory(path);
        std::filesystem::path watch_path = is_dir ? path : path.parent_path();
        std::filesystem::path file_name = is_dir ? "" : path.filename();

        int wd = inotify_add_watch(inotify_fd, watch_path.c_str(),
                                   IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO);
        if (wd < 0) {
            FJELL_CORE_WARN("FileWatcher: failed to watch {}", path.string());
            return false;
        }

        // If this wd already exists (same directory watched again), inotify
        // returns the same wd. We overwrite — last callback wins for that wd.
        // For multi-file watches on the same dir, we'd need a different approach,
        // but each consumer (shader/ui/script) watches different dirs.
        watches[wd] = {watch_path, file_name, std::move(callback)};
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

            auto it = watches.find(event->wd);
            if (it != watches.end()) {
                const auto& entry = it->second;
                std::filesystem::path changed_file =
                    (event->len > 0) ? std::filesystem::path(event->name) : entry.file_name;

                // If watching a specific file, only fire for that file
                if (entry.file_name.empty() || changed_file == entry.file_name) {
                    entry.callback(entry.base_path / changed_file);
                    any_fired = true;
                }
            }

            ptr += sizeof(inotify_event) + event->len;
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

} // namespace fjell::platform
