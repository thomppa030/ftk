#include "core/asset_remap.hpp"

#include "ftk/base/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace fjell {

namespace fs = std::filesystem;

AssetRemap::AssetRemap(fs::path project_root)
    : db_path_{project_root / ".fjell" / "asset_remap.json"} {}

ftk::Result<> AssetRemap::load() {
    if (!fs::exists(db_path_)) return {};

    std::ifstream file(db_path_);
    if (!file.is_open()) {
        return ftk::make_error("Failed to open asset remap: " + db_path_.string());
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        return ftk::make_error("Asset remap parse error: " + std::string(e.what()));
    }

    entries_.clear();
    if (!j.contains("remaps") || !j["remaps"].is_array()) return {};

    for (const auto& row : j["remaps"]) {
        if (!row.contains("from") || !row.contains("to")) continue;
        auto from = AssetId::from_string(row["from"].get<std::string>());
        auto to = AssetId::from_string(row["to"].get<std::string>());
        if (from.valid() && to.valid() && from != to) {
            entries_[from] = to;
        }
    }

    dirty_ = false;
    return {};
}

ftk::Result<> AssetRemap::save() const {
    if (entries_.empty() && !fs::exists(db_path_)) {
        dirty_ = false;
        return {};
    }

    std::error_code ec;
    fs::create_directories(db_path_.parent_path(), ec);
    if (ec) {
        return ftk::make_error("Failed to create .fjell directory: " + ec.message());
    }

    nlohmann::json j;
    j["version"] = 1;
    auto& remaps = j["remaps"];
    remaps = nlohmann::json::array();
    for (const auto& [from, to] : entries_) {
        remaps.push_back({{"from", from.to_string()}, {"to", to.to_string()}});
    }

    auto tmp = db_path_;
    tmp += ".tmp";
    {
        std::ofstream file(tmp);
        if (!file.is_open()) {
            return ftk::make_error("Failed to write asset remap: " + tmp.string());
        }
        file << j.dump(2) << "\n";
        if (!file) {
            return ftk::make_error("Failed to write asset remap: " + tmp.string());
        }
    }

    fs::rename(tmp, db_path_, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return ftk::make_error("Failed to replace asset remap: " + ec.message());
    }

    dirty_ = false;
    return {};
}

void AssetRemap::add(const AssetId& from, const AssetId& to) {
    if (!from.valid() || !to.valid() || from == to) return;

    // Anything that pointed at `from` follows it, so resolving stays one hop
    // no matter how many times an asset is superseded.
    for (auto& [_, target] : entries_) {
        if (target == from) target = to;
    }

    // Superseding something by an asset that already redirects here would
    // close a loop; drop the old entry instead.
    if (auto it = entries_.find(to); it != entries_.end() && it->second == from) {
        entries_.erase(it);
    }

    entries_[from] = to;
    dirty_ = true;
}

AssetId AssetRemap::resolve(const AssetId& id) const {
    auto it = entries_.find(id);
    return it != entries_.end() ? it->second : AssetId{};
}

void AssetRemap::remove(const AssetId& from) {
    if (entries_.erase(from) > 0) dirty_ = true;
}

} // namespace fjell
