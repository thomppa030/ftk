#pragma once

// Where an id points after the asset it named is gone.
//
// Repointing references at a replacement fixes them at the source, which is
// the outcome worth having. It can only fix references it can reach, though —
// not one in an unloaded scene, another branch, or a file edited outside the
// editor. This is the fallback for those: consulted only when an id misses, so
// a reference that was never rewritten still resolves.
//
// It also covers "this asset supersedes that one" without touching every
// referencing file at once.

#include "core/asset_id.hpp"
#include "core/result.hpp"

#include <filesystem>
#include <unordered_map>

namespace fjell {

class AssetRemap {
public:
    explicit AssetRemap(std::filesystem::path project_root);

    /// Load from .fjell/asset_remap.json. A missing file is not an error.
    Result<> load();

    /// Save if anything changed since the last save.
    Result<> save() const;

    /// Record that `from` now resolves to `to`. Existing entries pointing at
    /// `from` are re-pointed at `to` as well, so a chain of supersessions
    /// stays one hop and cannot become a cycle.
    void add(const AssetId& from, const AssetId& to);

    /// Where `id` ends up, or an invalid id if it is not remapped. A single
    /// lookup, never a walk: add() keeps chains collapsed to one hop, and
    /// resolving in one step means even a table hand-edited into a cycle
    /// cannot loop here.
    [[nodiscard]] AssetId resolve(const AssetId& id) const;

    /// Drop an entry, for when its references have been rewritten.
    void remove(const AssetId& from);

    [[nodiscard]] std::size_t size() const { return entries_.size(); }
    [[nodiscard]] bool dirty() const { return dirty_; }

    template <typename Fn>
    void for_each(const Fn& fn) const {
        for (const auto& [from, to] : entries_) fn(from, to);
    }

private:
    std::filesystem::path db_path_;
    std::unordered_map<AssetId, AssetId> entries_;
    mutable bool dirty_{false};
};

} // namespace fjell
