#include "core/asset_database.hpp"
#include "core/asset_identity.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <unordered_set>

namespace fjell {

namespace fs = std::filesystem;

AssetDatabase::AssetDatabase(fs::path project_root)
    : project_root_(std::move(project_root))
    , db_path_(project_root_ / ".fjell" / "asset_db.json") {}

// ── Persistence ─────────────────────────────────────────────────────────

Result<> AssetDatabase::load() {
    std::unique_lock lock(mutex_);

    if (!fs::exists(db_path_)) {
        return {};  // fresh project, nothing to load
    }

    std::ifstream file(db_path_);
    if (!file.is_open()) {
        return make_error("Failed to open asset database: " + db_path_.string());
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error("Asset database parse error: " + std::string(e.what()));
    }

    by_id_.clear();
    by_path_.clear();

    if (!j.contains("assets") || !j["assets"].is_array()) {
        return {};
    }

    for (const auto& entry : j["assets"]) {
        if (!entry.contains("uuid") || !entry.contains("path")) continue;
        auto id = AssetId::from_string(entry["uuid"].get<std::string>());
        auto path = entry["path"].get<std::string>();
        if (id.valid() && !path.empty()) {
            if (entry.contains("stamp")) {
                stamps_[path] = entry["stamp"].get<std::string>();
            }
            insert_locked(id, std::move(path));
        }
    }

    dirty_ = false;
    return {};
}

Result<> AssetDatabase::save() const {
    std::shared_lock lock(mutex_);

    // Ensure .fjell/ directory exists
    auto dir = db_path_.parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return make_error("Failed to create .fjell directory: " + ec.message());
    }

    nlohmann::json j;
    j["version"] = 1;

    auto& assets = j["assets"];
    assets = nlohmann::json::array();
    for (const auto& [id, entry] : by_id_) {
        nlohmann::json row{
            {"uuid", entry.id.to_string()},
            {"path", entry.relative_path},
        };
        // Lets the next rebuild skip re-reading an unchanged file.
        if (auto it = stamps_.find(entry.relative_path); it != stamps_.end()) {
            row["stamp"] = it->second;
        }
        assets.push_back(std::move(row));
    }

    // Atomic write: write to .tmp, then rename
    auto tmp_path = db_path_;
    tmp_path += ".tmp";

    {
        std::ofstream out(tmp_path);
        if (!out.is_open()) {
            return make_error("Failed to write asset database: " + tmp_path.string());
        }
        out << j.dump(2) << '\n';
    }

    fs::rename(tmp_path, db_path_, ec);
    if (ec) {
        return make_error("Failed to rename asset database: " + ec.message());
    }

    return {};
}

// ── Rebuild ─────────────────────────────────────────────────────────────

namespace {

/// Size and mtime, enough to tell "same file as last time" from "changed".
std::string file_stamp(const fs::directory_entry& entry) {
    std::error_code ec;
    auto size = entry.file_size(ec);
    if (ec) return {};
    auto time = entry.last_write_time(ec);
    if (ec) return {};
    return std::to_string(size) + ":" +
           std::to_string(time.time_since_epoch().count());
}

} // namespace

void AssetDatabase::rebuild() {
    std::unique_lock lock(mutex_);

    // Track which paths we see on disk
    std::unordered_set<std::string> seen_paths;

    // Assets not yet carrying their id. Written after the walk so the
    // directory iterator doesn't see files created underneath it.
    std::vector<std::pair<fs::path, AssetId>> needs_write;

    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(project_root_, ec)) {
        if (!entry.is_regular_file()) continue;

        // generic_string so the separator checks below hold on every platform.
        auto rel = fs::relative(entry.path(), project_root_, ec).generic_string();
        if (ec) continue;

        // Skip engine metadata, build dirs, hidden dirs
        if (rel.starts_with(".fjell") || rel.starts_with("build") ||
            rel.starts_with(".") || rel.find("/.") != std::string::npos) {
            continue;
        }

        // Sidecars carry other files' identities; they are not assets.
        if (asset_identity::is_sidecar(entry.path())) continue;

        auto ext = entry.path().extension().string();
        if (!is_trackable(ext)) continue;

        seen_paths.insert(rel);

        // An asset still sitting where the database last saw it keeps its id,
        // and reading the file to confirm would mean opening every asset in
        // the project on every startup. Only a path the database does not
        // recognise is worth inspecting.
        if (auto it = by_path_.find(rel); it != by_path_.end()) {
            auto stamp = file_stamp(entry);
            auto known = stamps_.find(rel);
            if (known != stamps_.end() && known->second == stamp) {
                continue;
            }

            // Changed since last seen: it may have been overwritten by a
            // different asset (a restore, a checkout, a copy over the top),
            // so the id it carries decides.
            AssetId carried = asset_identity::read(entry.path());
            if (carried.valid() && carried != it->second) {
                remove_locked(it->second);
            } else {
                if (!carried.valid()) {
                    // Tracked but carrying nothing yet — give it the id it
                    // already has so the next move preserves it.
                    needs_write.emplace_back(entry.path(), it->second);
                }
                // Worth persisting: it is what lets the next rebuild skip
                // opening this file at all.
                stamps_[rel] = stamp;
                dirty_ = true;
                continue;
            }
        }

        // The id a file carries is what survives it being moved or renamed
        // outside the editor, which is exactly when the path map is wrong.
        AssetId carried = asset_identity::read(entry.path());

        AssetId id = carried;

        if (id.valid()) {
            if (auto it = by_id_.find(id); it != by_id_.end()) {
                bool original_gone = !fs::exists(project_root_ / it->second.relative_path, ec);
                if (original_gone) {
                    // The file this id belongs to is no longer where it was:
                    // it moved, and this is it. Repoint rather than treating
                    // its own id as a clash.
                    stamps_.erase(it->second.relative_path);
                    by_path_.erase(it->second.relative_path);
                    it->second.relative_path = rel;
                    by_path_[rel] = id;
                    stamps_[rel] = file_stamp(entry);
                    dirty_ = true;
                    continue;
                }
                // The original is still there, so this is a copy carrying a
                // duplicated id. Copies are new assets and need their own.
                id = AssetId::generate();
                while (by_id_.contains(id)) {
                    id = AssetId::generate();
                }
            }
        } else {
            id = AssetId::generate();
            // Two files can only collide here if one was copied without its
            // id, so a fresh one is the right answer.
            while (by_id_.contains(id)) {
                id = AssetId::generate();
            }
        }

        insert_locked(id, rel);
        stamps_[rel] = file_stamp(entry);
        if (id != carried) {
            needs_write.emplace_back(entry.path(), id);
        }
        dirty_ = true;
    }

    // Remove entries for files that no longer exist
    std::vector<AssetId> stale;
    for (const auto& [id, entry] : by_id_) {
        if (!seen_paths.contains(entry.relative_path)) {
            stale.push_back(id);
        }
    }
    for (const auto& id : stale) {
        remove_locked(id);
        dirty_ = true;
    }

    if (!stale.empty()) {
        FJELL_CORE_INFO("Asset database: removed {} stale entries", stale.size());
    }

    // Store ids into the assets themselves. A failure here leaves the entry in
    // the database, so the asset still loads — it just won't survive a move.
    uint32_t written = 0;
    for (const auto& [path, id] : needs_write) {
        if (asset_identity::write(path, id)) {
            ++written;
        }
    }
    if (written > 0) {
        FJELL_CORE_INFO("Asset database: stored ids in {} assets", written);
    }
}

// ── Lookups ─────────────────────────────────────────────────────────────

const AssetEntry* AssetDatabase::find(const AssetId& id) const {
    std::shared_lock lock(mutex_);
    auto it = by_id_.find(id);
    return it != by_id_.end() ? &it->second : nullptr;
}

const AssetEntry* AssetDatabase::find_by_path(const std::string& relative_path) const {
    std::shared_lock lock(mutex_);
    auto it = by_path_.find(relative_path);
    if (it == by_path_.end()) return nullptr;
    auto entry_it = by_id_.find(it->second);
    return entry_it != by_id_.end() ? &entry_it->second : nullptr;
}

// ── Mutations ───────────────────────────────────────────────────────────

AssetId AssetDatabase::register_asset(const std::string& relative_path) {
    std::unique_lock lock(mutex_);

    // Already tracked?
    auto it = by_path_.find(relative_path);
    if (it != by_path_.end()) {
        return it->second;
    }

    auto id = AssetId::generate();
    insert_locked(id, relative_path);
    dirty_ = true;
    return id;
}

void AssetDatabase::notify_move(const AssetId& id, const std::string& new_relative_path) {
    std::unique_lock lock(mutex_);

    auto it = by_id_.find(id);
    if (it == by_id_.end()) return;

    // Remove old path mapping
    by_path_.erase(it->second.relative_path);

    // The stamp describes the same file, so it follows the path it is keyed by.
    auto stamp = stamps_.extract(it->second.relative_path);

    // Update
    it->second.relative_path = new_relative_path;
    by_path_[new_relative_path] = id;
    if (!stamp.empty()) {
        stamps_[new_relative_path] = std::move(stamp.mapped());
    }
    dirty_ = true;
}

void AssetDatabase::remove(const AssetId& id) {
    std::unique_lock lock(mutex_);
    remove_locked(id);
    dirty_ = true;
}

std::size_t AssetDatabase::size() const {
    std::shared_lock lock(mutex_);
    return by_id_.size();
}

// ── Helpers ─────────────────────────────────────────────────────────────

bool AssetDatabase::is_trackable(const std::string& ext) {
    static const std::unordered_set<std::string> exts = {
        // Raw resources
        ".png", ".jpg", ".jpeg", ".exr", ".hdr",
        ".glb", ".gltf",
        ".wav", ".mp3", ".ogg",
        // Fjell formats
        ".fjmat", ".fjvfx", ".fjanimset", ".fjblend1D", ".fjblend2D",
        ".fjaction", ".fjinput", ".fjanim", ".fjaudio",
        ".fjday",
        ".fjwidget", ".fjui", ".fjss",
        ".fjsl", ".fjp", ".fjell",
        // Binary Fjell formats
        ".fjmesh", ".fjskel",
    };
    return exts.contains(ext);
}

void AssetDatabase::insert_locked(AssetId id, std::string relative_path) {
    by_path_[relative_path] = id;
    by_id_[id] = AssetEntry{id, std::move(relative_path)};
}

void AssetDatabase::remove_locked(const AssetId& id) {
    auto it = by_id_.find(id);
    if (it == by_id_.end()) return;
    stamps_.erase(it->second.relative_path);
    by_path_.erase(it->second.relative_path);
    by_id_.erase(it);
}

} // namespace fjell
