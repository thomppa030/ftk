#include "core/asset_database.hpp"
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

        auto ext = entry.path().extension().string();
        if (!is_trackable(ext)) continue;

        seen_paths.insert(rel);

        // Already tracked?
        if (by_path_.contains(rel)) continue;

        // Try to recover UUID from embedded __uuid in Fjell JSON formats
        AssetId id;
        if (is_fjell_json(ext)) {
            id = read_inline_uuid(entry.path());
        }

        // If no embedded UUID (or not a JSON format), generate a new one
        if (!id.valid()) {
            id = AssetId::generate();
        }

        // Check for UUID collision (astronomically unlikely, but be safe)
        while (by_id_.contains(id)) {
            id = AssetId::generate();
        }

        insert_locked(id, rel);
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
