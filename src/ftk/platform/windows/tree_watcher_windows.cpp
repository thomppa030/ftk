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

#include <optional>
#include <string>
#include <vector>

namespace ftk::platform {

struct TreeWatcher::Impl {
    std::filesystem::path root;
    SkipFolder skip;
    HANDLE handle{INVALID_HANDLE_VALUE};
    OVERLAPPED overlapped{};
    // DWORD-aligned, as the notifications laid in it must be.
    std::vector<DWORD> buffer = std::vector<DWORD>(16384);
    bool pending{false};
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
        const BOOL ok = ReadDirectoryChangesW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(DWORD)), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE, nullptr,
            &overlapped, nullptr);
        pending = ok != FALSE;
        return pending;
    }

    bool watch(const std::filesystem::path& folder, SkipFolder skip_folder) {
        stop();
        root = folder;
        skip = std::move(skip_folder);
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
        while (pending && WaitForSingleObject(overlapped.hEvent, 0) == WAIT_OBJECT_0) {
            DWORD bytes = 0;
            if (GetOverlappedResult(handle, &overlapped, &bytes, FALSE) == FALSE) {
                pending = false;
                break;
            }
            // A buffer too small for what happened comes back empty.
            if (bytes == 0) events.push_back({.kind = Kind::lost, .path = root});

            const auto* bytes_start = reinterpret_cast<const char*>(buffer.data());
            for (const char* at = bytes_start; bytes > 0;) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(at);
                const std::filesystem::path relative(
                    std::wstring(info->FileName, info->FileNameLength / sizeof(wchar_t)));
                const std::filesystem::path path = root / relative;
                std::error_code ec;
                const bool folder = std::filesystem::is_directory(path, ec);

                if (!skipped(relative, folder)) {
                    switch (info->Action) {
                        case FILE_ACTION_ADDED:
                            events.push_back({.kind = Kind::added, .path = path, .folder = folder});
                            if (folder) report_contents(path, events);
                            break;
                        case FILE_ACTION_MODIFIED:
                            // A folder is reported modified when what is in it
                            // changes, which its files' events tell already.
                            if (!folder) events.push_back({.kind = Kind::changed, .path = path});
                            break;
                        case FILE_ACTION_REMOVED:
                            events.push_back({.kind = Kind::removed, .path = path});
                            break;
                        case FILE_ACTION_RENAMED_OLD_NAME:
                            renamed_from = path;
                            break;
                        case FILE_ACTION_RENAMED_NEW_NAME:
                            if (renamed_from) {
                                events.push_back(
                                    {.kind = Kind::renamed, .path = path, .from = *renamed_from, .folder = folder});
                            } else {
                                events.push_back({.kind = Kind::added, .path = path, .folder = folder});
                            }
                            renamed_from.reset();
                            break;
                        default:
                            break;
                    }
                }
                if (info->NextEntryOffset == 0) break;
                at += info->NextEntryOffset;
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
