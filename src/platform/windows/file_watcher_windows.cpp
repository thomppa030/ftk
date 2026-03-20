#include "platform/file_watcher.hpp"
#include "core/log.hpp"

#ifndef _WIN32
#error "This file should only be compiled on Windows"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <unordered_map>

namespace fjell::platform {

struct FileWatcher::Impl {
    struct WatchEntry {
        HANDLE dir_handle{INVALID_HANDLE_VALUE};
        OVERLAPPED overlapped{};
        std::array<char, 4096> buffer{};
        std::filesystem::path dir_path;
        std::filesystem::path file_filter; // empty = watch all files in dir
        Callback callback;
        bool pending{false};
    };

    std::vector<WatchEntry> watches;

    ~Impl() { clear(); }

    void clear() {
        for (auto& w : watches) {
            if (w.pending) {
                CancelIo(w.dir_handle);
            }
            if (w.dir_handle != INVALID_HANDLE_VALUE) {
                CloseHandle(w.dir_handle);
            }
            if (w.overlapped.hEvent != nullptr) {
                CloseHandle(w.overlapped.hEvent);
            }
        }
        watches.clear();
    }

    bool begin_read(WatchEntry& w) {
        BOOL ok = ReadDirectoryChangesW(
            w.dir_handle,
            w.buffer.data(),
            static_cast<DWORD>(w.buffer.size()),
            FALSE, // don't watch subtree
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME,
            nullptr,
            &w.overlapped,
            nullptr);
        w.pending = (ok != FALSE);
        return w.pending;
    }

    bool add_watch(const std::filesystem::path& path, Callback callback) {
        bool is_dir = std::filesystem::is_directory(path);
        std::filesystem::path dir_path = is_dir ? path : path.parent_path();
        std::filesystem::path file_filter = is_dir ? "" : path.filename();

        HANDLE h = CreateFileW(
            dir_path.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);

        if (h == INVALID_HANDLE_VALUE) {
            FJELL_CORE_WARN("FileWatcher: failed to open directory {}", dir_path.string());
            return false;
        }

        WatchEntry w;
        w.dir_handle = h;
        w.dir_path = dir_path;
        w.file_filter = file_filter;
        w.callback = std::move(callback);
        w.overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        if (!begin_read(w)) {
            CloseHandle(w.overlapped.hEvent);
            CloseHandle(h);
            FJELL_CORE_WARN("FileWatcher: ReadDirectoryChangesW failed for {}", dir_path.string());
            return false;
        }

        watches.push_back(std::move(w));
        return true;
    }

    bool poll_events() {
        bool any_fired = false;

        for (auto& w : watches) {
            if (!w.pending) continue;

            DWORD result = WaitForSingleObject(w.overlapped.hEvent, 0);
            if (result != WAIT_OBJECT_0) continue;

            DWORD bytes_transferred = 0;
            if (!GetOverlappedResult(w.dir_handle, &w.overlapped, &bytes_transferred, FALSE)) {
                w.pending = false;
                continue;
            }

            if (bytes_transferred > 0) {
                const char* ptr = w.buffer.data();
                while (true) {
                    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);

                    if (info->Action == FILE_ACTION_MODIFIED ||
                        info->Action == FILE_ACTION_ADDED ||
                        info->Action == FILE_ACTION_RENAMED_NEW_NAME) {

                        // Convert wide filename to path
                        std::wstring wide_name(info->FileName,
                                               info->FileNameLength / sizeof(wchar_t));
                        std::filesystem::path changed_file(wide_name);

                        if (w.file_filter.empty() || changed_file.filename() == w.file_filter) {
                            w.callback(w.dir_path / changed_file);
                            any_fired = true;
                        }
                    }

                    if (info->NextEntryOffset == 0) break;
                    ptr += info->NextEntryOffset;
                }
            }

            // Re-arm the watch
            ResetEvent(w.overlapped.hEvent);
            begin_read(w);
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
