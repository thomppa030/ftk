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
        assets.push_back({
            {"uuid", entry.id.to_string()},
            {"path", entry.relative_path},
        });
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

        auto rel = fs::relative(entry.path(), project_root_, ec).string();
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

        // The id the asset carries is authoritative — it is what survives the
        // file being moved or renamed outside the editor, which is exactly the
        // case where the path map is already wrong.
        AssetId carried = asset_identity::read(entry.path());

        if (auto it = by_path_.find(rel); it != by_path_.end()) {
            if (carried.valid() && carried != it->second) {
                // The file was replaced by one with its own identity (a copy,
                // a restore, a checkout). Its id wins over the stale mapping.
                remove_locked(it->second);
            } else {
                if (!carried.valid()) {
                    // Tracked but carrying nothing yet — give it the id it
                    // already has so the next move preserves it.
                    needs_write.emplace_back(entry.path(), it->second);
                }
                continue;
            }
        }

        AssetId id = carried;

        if (id.valid()) {
            if (auto it = by_id_.find(id); it != by_id_.end()) {
                bool original_gone = !fs::exists(project_root_ / it->second.relative_path, ec);
                if (original_gone) {
                    // The file this id belongs to is no longer where it was:
                    // it moved, and this is it. Repoint rather than treating
                    // its own id as a clash.
                    by_path_.erase(it->second.relative_path);
                    it->second.relative_path = rel;
                    by_path_[rel] = id;
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

    // Update
    it->second.relative_path = new_relative_path;
    by_path_[new_relative_path] = id;
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
        ".fjwidget", ".fjui", ".fjss",
        ".fjsl", ".fjp", ".fjell",
        // Binary Fjell formats
        ".fjmesh", ".fjskel",
    };
    return exts.contains(ext);
}

bool AssetDatabase::is_fjell_json(const std::string& ext) {
    static const std::unordered_set<std::string> exts = {
        ".fjmat", ".fjvfx", ".fjanimset", ".fjblend1D", ".fjblend2D",
        ".fjaction", ".fjinput", ".fjanim", ".fjaudio",
        ".fjwidget", ".fjp", ".fjell",
    };
    return exts.contains(ext);
}

AssetId AssetDatabase::read_inline_uuid(const fs::path& file) {
    std::ifstream in(file);
    if (!in.is_open()) return {};

    // Read enough to find __uuid near the top of the file.
    // Don't parse the entire file — just look for the field.
    std::string line;
    int lines_read = 0;
    while (std::getline(in, line) && lines_read < 10) {
        auto pos = line.find("\"__uuid\"");
        if (pos != std::string::npos) {
            // Find the value — look for the next quoted string
            auto quote1 = line.find('"', pos + 8);
            if (quote1 == std::string::npos) return {};
            auto quote2 = line.find('"', quote1 + 1);
            if (quote2 == std::string::npos) return {};
            auto val = line.substr(quote1 + 1, quote2 - quote1 - 1);
            return AssetId::from_string(val);
        }
        ++lines_read;
    }
    return {};
}

void AssetDatabase::insert_locked(AssetId id, std::string relative_path) {
    by_path_[relative_path] = id;
    by_id_[id] = AssetEntry{id, std::move(relative_path)};
}

void AssetDatabase::remove_locked(const AssetId& id) {
    auto it = by_id_.find(id);
    if (it == by_id_.end()) return;
    by_path_.erase(it->second.relative_path);
    by_id_.erase(it);
}

} // namespace fjell
