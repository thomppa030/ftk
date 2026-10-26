#include "ftk/platform/tree_watcher.hpp"
#include "ftk/base/log.hpp"

#ifndef _WIN32
#error "This file should only be compiled on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ftk::platform {

namespace {

constexpr DWORD WATCHED_CHANGES =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE;

// One notification as either kind of read lays it out.
struct Notice {
    DWORD action{0};
    std::filesystem::path relative;
    // The file's id on its volume; 0 where the read doesn't tell it.
    int64_t file_id{0};
    // Whether it is a folder, as it was when the change was made, a removed
    // one's too; none where the read doesn't tell it.
    std::optional<bool> folder;
    DWORD next{0};
};

} // namespace

struct TreeWatcher::Impl {
    std::filesystem::path root;
    SkipFolder skip;
    HANDLE handle{INVALID_HANDLE_VALUE};
    OVERLAPPED overlapped{};
    // DWORD-aligned, as the notifications laid in it must be.
    std::vector<DWORD> buffer = std::vector<DWORD>(16384);
    bool pending{false};
    // Whether the reads carry each file's id (ReadDirectoryChangesExW's
    // extended notifications, Windows 10 1709 on), which make a file moved
    // between folders, told as removed from one and added to the other, one
    // rename. A volume that doesn't give them is read the plain way.
    bool extended{true};
    // A rename's old name, waiting for its new one.
    std::optional<std::filesystem::path> renamed_from;

    ~Impl() { stop(); }

    void stop() {
        if (pending) {
            CancelIo(handle);
            DWORD ignored = 0;
            GetOverlappedResult(handle, &overlapped, &ignored, TRUE);
        }
        pending = false;
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
        if (overlapped.hEvent != nullptr) CloseHandle(overlapped.hEvent);
        overlapped = {};
        renamed_from.reset();
    }

    bool begin_read() {
        const auto size = static_cast<DWORD>(buffer.size() * sizeof(DWORD));
        BOOL ok = FALSE;
        if (extended) {
            ok = ReadDirectoryChangesExW(handle, buffer.data(), size, TRUE, WATCHED_CHANGES, nullptr, &overlapped,
                                         nullptr, ReadDirectoryNotifyExtendedInformation);
            extended = ok != FALSE;
        }
        if (!extended) {
            ok = ReadDirectoryChangesW(handle, buffer.data(), size, TRUE, WATCHED_CHANGES, nullptr, &overlapped,
                                       nullptr);
        }
        pending = ok != FALSE;
        return pending;
    }

    // The notification at `at` in the buffer, laid out as the read asked.
    [[nodiscard]] Notice notice_at(const char* at) const {
        if (extended) {
            // Copied out rather than read in place: the extended layout holds
            // 8-byte fields, and the buffer is only sure to be DWORD-aligned.
            FILE_NOTIFY_EXTENDED_INFORMATION info{};
            std::memcpy(&info, at, offsetof(FILE_NOTIFY_EXTENDED_INFORMATION, FileName));
            const auto* name = reinterpret_cast<const wchar_t*>(at + offsetof(FILE_NOTIFY_EXTENDED_INFORMATION, FileName));
            return {.action = info.Action,
                    .relative = std::wstring(name, info.FileNameLength / sizeof(wchar_t)),
                    .file_id = info.FileId.QuadPart,
                    .folder = (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                    .next = info.NextEntryOffset};
        }
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(at);
        return {.action = info->Action,
                .relative = std::wstring(info->FileName, info->FileNameLength / sizeof(wchar_t)),
                .next = info->NextEntryOffset};
    }

    bool watch(const std::filesystem::path& folder, SkipFolder skip_folder) {
        stop();
        root = folder;
        skip = std::move(skip_folder);
        extended = true;
        handle = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            FTK_CORE_WARN("TreeWatcher: {} could not be opened", folder.string());
            return false;
        }
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!begin_read()) {
            FTK_CORE_WARN("TreeWatcher: {} could not be watched", folder.string());
            stop();
            return false;
        }
        return true;
    }

    // Whether `relative` passes through a folder the watcher leaves alone, or
    // is one.
    [[nodiscard]] bool skipped(const std::filesystem::path& relative, bool folder) const {
        if (!skip) return false;
        const std::filesystem::path within = folder ? relative : relative.parent_path();
        for (const auto& part : within) {
            if (skip(part.string())) return true;
        }
        return false;
    }

    // A folder that arrived with contents: its files and folders are added
    // too, as Windows tells of the folder alone when one is moved in.
    void report_contents(const std::filesystem::path& folder, std::vector<TreeEvent>& events) const {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(folder, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            const bool is_folder = it->is_directory(ec);
            if (is_folder && skip && skip(it->path().filename().string())) {
                it.disable_recursion_pending();
                continue;
            }
            events.push_back({.kind = TreeEvent::Kind::added, .path = it->path(), .folder = is_folder});
        }
    }

    std::vector<TreeEvent> poll() {
        using Kind = TreeEvent::Kind;
        std::vector<TreeEvent> events;
        // Files removed this poll, by id, at their place in `events`: one
        // added under the same id was moved there, and the two are a rename.
        std::unordered_map<int64_t, size_t> removed_at;
        while (pending && WaitForSingleObject(overlapped.hEvent, 0) == WAIT_OBJECT_0) {
            DWORD bytes = 0;
            if (GetOverlappedResult(handle, &overlapped, &bytes, FALSE) == FALSE) {
                // An extended read the volume turned down is asked again the
                // plain way.
                pending = false;
                if (!extended) break;
                extended = false;
                ResetEvent(overlapped.hEvent);
                if (!begin_read()) break;
                continue;
            }
            // A buffer too small for what happened comes back empty.
            if (bytes == 0) events.push_back({.kind = Kind::lost, .path = root});

            const auto* bytes_start = reinterpret_cast<const char*>(buffer.data());
            for (const char* at = bytes_start; bytes > 0;) {
                const Notice notice = notice_at(at);
                const std::filesystem::path path = root / notice.relative;
                // Asked of the disk only where the read doesn't say: a removed
                // folder is no longer there to ask, nor is one added and
                // removed again before the poll.
                std::error_code ec;
                const bool folder = notice.folder ? *notice.folder : std::filesystem::is_directory(path, ec);
                const bool hidden = skipped(notice.relative, folder);

                switch (notice.action) {
                    case FILE_ACTION_ADDED: {
                        if (hidden) break;
                        const auto moved = notice.file_id != 0 ? removed_at.find(notice.file_id) : removed_at.end();
                        if (moved != removed_at.end()) {
                            TreeEvent& gone = events[moved->second];
                            std::filesystem::path from = std::move(gone.path);
                            gone = {.kind = Kind::renamed, .path = path, .from = std::move(from), .folder = folder};
                            removed_at.erase(moved);
                            break;
                        }
                        events.push_back({.kind = Kind::added, .path = path, .folder = folder});
                        if (folder) report_contents(path, events);
                        break;
                    }
                    case FILE_ACTION_MODIFIED:
                        // A folder is reported modified when what is in it
                        // changes, which its files' events tell already.
                        if (!hidden && !folder) events.push_back({.kind = Kind::changed, .path = path});
                        break;
                    case FILE_ACTION_REMOVED:
                        if (hidden) break;
                        if (notice.file_id != 0) removed_at[notice.file_id] = events.size();
                        events.push_back({.kind = Kind::removed, .path = path, .folder = folder});
                        break;
                    case FILE_ACTION_RENAMED_OLD_NAME:
                        // From a folder left alone, it arrives as new.
                        if (hidden) {
                            renamed_from.reset();
                        } else {
                            renamed_from = path;
                        }
                        break;
                    case FILE_ACTION_RENAMED_NEW_NAME:
                        if (hidden) {
                            // Into a folder left alone: gone from the tree.
                            if (renamed_from) {
                                events.push_back({.kind = Kind::removed, .path = *renamed_from, .folder = folder});
                            }
                        } else if (renamed_from) {
                            events.push_back(
                                {.kind = Kind::renamed, .path = path, .from = *renamed_from, .folder = folder});
                        } else {
                            events.push_back({.kind = Kind::added, .path = path, .folder = folder});
                            if (folder) report_contents(path, events);
                        }
                        renamed_from.reset();
                        break;
                    default:
                        break;
                }
                if (notice.next == 0) break;
                at += notice.next;
            }
            ResetEvent(overlapped.hEvent);
            begin_read();
        }
        return tidy_tree_events(std::move(events));
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
