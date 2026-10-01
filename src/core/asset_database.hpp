#pragma once

#include "core/asset_id.hpp"
#include "ftk/base/result.hpp"

#include <filesystem>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

struct AssetEntry {
    AssetId id;
    std::string relative_path;  // relative to project root
};

class AssetDatabase {
public:
    explicit AssetDatabase(std::filesystem::path project_root);

    /// Load from .fjell/asset_db.json. Creates empty db if file doesn't exist.
    Result<> load();

    /// Save to .fjell/asset_db.json. Atomic write (tmp + rename).
    Result<> save() const;

    /// Walk the project, assign UUIDs to new files, remove stale entries,
    /// recover UUIDs from __uuid fields in Fjell JSON formats.
    void rebuild();

    /// Look up by UUID (thread-safe read).
    [[nodiscard]] const AssetEntry* find(const AssetId& id) const;

    /// Look up by relative path (thread-safe read).
    [[nodiscard]] const AssetEntry* find_by_path(const std::string& relative_path) const;

    /// Register a new asset. Returns the assigned UUID.
    AssetId register_asset(const std::string& relative_path);

    /// Update path for a known asset (rename/move).
    void notify_move(const AssetId& id, const std::string& new_relative_path);

    /// Remove an entry (asset deleted).
    void remove(const AssetId& id);

    /// Number of tracked assets.
    [[nodiscard]] std::size_t size() const;

    [[nodiscard]] bool dirty() const { return dirty_; }

    template <typename Fn>
    void for_each(const Fn& fn) const {
        std::shared_lock lock(mutex_);
        for (const auto& [id, entry] : by_id_) {
            fn(entry);
        }
    }

private:
    std::filesystem::path project_root_;
    std::filesystem::path db_path_;

    std::unordered_map<AssetId, AssetEntry> by_id_;
    std::unordered_map<std::string, AssetId> by_path_;

    /// Size and mtime of each asset as of the last rebuild, so an unchanged
    /// file can keep its id without being opened. Reading every asset to
    /// confirm an id the database already has costs a full pass over the
    /// project's bytes on every startup.
    std::unordered_map<std::string, std::string> stamps_;

    bool dirty_{false};

    mutable std::shared_mutex mutex_;

    /// Check if a file extension should be tracked. Where an asset's id lives
    /// is asset_identity's business, not the database's.
    static bool is_trackable(const std::string& ext);

    /// Insert an entry into both maps. Caller holds exclusive lock.
    void insert_locked(AssetId id, std::string relative_path);

    /// Remove by id from both maps. Caller holds exclusive lock.
    void remove_locked(const AssetId& id);
};

} // namespace fjell
