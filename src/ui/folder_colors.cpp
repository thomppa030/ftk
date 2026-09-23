#include "ui/folder_colors.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <vector>

namespace fjell {

namespace fs = std::filesystem;

namespace {

/// `key` itself or a folder beneath it.
bool is_within(const std::string& candidate, const std::string& key) {
    return candidate == key ||
           (candidate.size() > key.size() && candidate.starts_with(key) &&
            candidate[key.size()] == '/');
}

} // namespace

void FolderColors::load(const fs::path& project_root) {
    root_ = project_root.lexically_normal();
    file_ = root_.empty() ? fs::path{} : root_ / ".fjell" / "folder_colors.json";
    colors_.clear();
    dirty_ = false;

    std::error_code ec;
    if (file_.empty() || !fs::exists(file_, ec)) return;

    std::ifstream in(file_);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.contains("folders") || !j["folders"].is_object()) {
        FJELL_CORE_WARN("Ignoring unreadable folder colours: {}", file_.string());
        return;
    }

    for (const auto& [folder, value] : j["folders"].items()) {
        auto color = value.is_string() ? from_hex(value.get<std::string>()) : std::nullopt;
        // A folder renamed or deleted outside the editor leaves its colour
        // behind; it is dropped here rather than kept forever.
        if (!color || !fs::is_directory(root_ / fs::path(folder), ec)) {
            dirty_ = true;
            continue;
        }
        colors_[folder] = *color;
    }
}

Result<> FolderColors::save() {
    if (!dirty_ || file_.empty()) return {};

    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    if (ec) return make_error("Failed to create .fjell directory: " + ec.message());

    nlohmann::json j;
    j["version"] = 1;
    auto& folders = j["folders"];
    folders = nlohmann::json::object();
    for (const auto& [folder, color] : colors_) {
        folders[folder] = to_hex(color);
    }

    // Written beside the real file and swapped in, so a crash mid-write
    // cannot leave half a file behind.
    auto tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        if (!out.is_open()) return make_error("Failed to write " + tmp.string());
        out << j.dump(2) << '\n';
    }
    fs::rename(tmp, file_, ec);
    if (ec) return make_error("Failed to replace " + file_.string() + ": " + ec.message());

    dirty_ = false;
    return {};
}

std::optional<glm::vec3> FolderColors::find(const fs::path& folder) const {
    auto it = colors_.find(key(folder));
    if (it == colors_.end()) return std::nullopt;
    return it->second;
}

void FolderColors::set(const fs::path& folder, const glm::vec3& color) {
    auto k = key(folder);
    if (k.empty()) return;
    auto it = colors_.find(k);
    if (it != colors_.end() && it->second == color) return;
    colors_[k] = color;
    dirty_ = true;
}

void FolderColors::clear(const fs::path& folder) {
    if (colors_.erase(key(folder)) > 0) dirty_ = true;
}

void FolderColors::moved(const fs::path& from, const fs::path& to) {
    auto from_key = key(from);
    if (from_key.empty()) return;
    auto to_key = key(to);

    std::vector<std::pair<std::string, glm::vec3>> carried;
    for (auto it = colors_.begin(); it != colors_.end();) {
        if (is_within(it->first, from_key)) {
            carried.emplace_back(it->first.substr(from_key.size()), it->second);
            it = colors_.erase(it);
        } else {
            ++it;
        }
    }
    if (carried.empty()) return;

    dirty_ = true;
    // Moved out of the project, the colours have nothing left to name.
    if (to_key.empty()) return;
    for (auto& [suffix, color] : carried) {
        colors_[to_key + suffix] = color;
    }
}

void FolderColors::removed(const fs::path& folder) {
    auto k = key(folder);
    if (k.empty()) return;
    auto erased = std::erase_if(colors_, [&](const auto& kv) { return is_within(kv.first, k); });
    if (erased > 0) dirty_ = true;
}

std::string FolderColors::to_hex(const glm::vec3& color) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::string hex = "#";
    for (int i = 0; i < 3; ++i) {
        auto byte = static_cast<int>(std::lround(std::clamp(color[i], 0.0f, 1.0f) * 255.0f));
        hex += DIGITS[byte >> 4];
        hex += DIGITS[byte & 0xF];
    }
    return hex;
}

std::optional<glm::vec3> FolderColors::from_hex(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') return std::nullopt;
    // from_chars would take a sign as part of a pair.
    if (!std::all_of(hex.begin() + 1, hex.end(),
                     [](unsigned char c) { return std::isxdigit(c) != 0; })) {
        return std::nullopt;
    }
    glm::vec3 color{};
    for (int i = 0; i < 3; ++i) {
        const char* first = hex.data() + 1 + i * 2;
        int byte = 0;
        auto [end, ec] = std::from_chars(first, first + 2, byte, 16);
        if (ec != std::errc{} || end != first + 2) return std::nullopt;
        color[i] = static_cast<float>(byte) / 255.0f;
    }
    return color;
}

std::string FolderColors::key(const fs::path& folder) const {
    if (root_.empty()) return {};
    auto rel = folder.lexically_normal().lexically_relative(root_);
    if (rel.empty() || rel == "." || *rel.begin() == "..") return {};
    auto k = rel.generic_string();
    // A trailing separator names the same folder.
    while (!k.empty() && k.back() == '/') k.pop_back();
    return k;
}

} // namespace fjell
