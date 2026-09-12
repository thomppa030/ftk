#include "core/asset_references.hpp"

#include "core/asset_database.hpp"
#include "core/asset_identity.hpp"
#include "core/log.hpp"

#include <fstream>
#include <unordered_set>

namespace fjell {

namespace fs = std::filesystem;

namespace {

/// Formats whose contents are text and can therefore name an id. Binary
/// formats keep references in their own sections, which the scan cannot read
/// — .fjmesh material slots are the one case, and the mesh editor rewrites
/// those itself.
bool is_text_asset(const std::string& ext) {
    static const std::unordered_set<std::string> exts = {
        ".fjmat", ".fjvfx", ".fjanimset", ".fjblend1D", ".fjblend2D",
        ".fjaction", ".fjinput", ".fjanim", ".fjaudio",
        ".fjp", ".fjell", ".fjui", ".fjss", ".fjwidget", ".fjgraph",
    };
    return exts.contains(ext);
}

uint32_t count_occurrences(const std::string& haystack, const std::string& needle) {
    uint32_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

constexpr size_t CHUNK = 64 * 1024;

/// Whether a file mentions `needle`, without reading more of it than needed.
/// Assets that carry bulk data — an animation clip is mostly keyframes — are
/// far larger than the reference near the top, and answering "is it in here"
/// for the whole project must not mean reading the whole project.
bool file_contains(const fs::path& path, const std::string& needle) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;

    std::string window;
    std::string buffer(CHUNK, '\0');
    while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           in.gcount() > 0) {
        window.append(buffer, 0, static_cast<size_t>(in.gcount()));
        if (window.find(needle) != std::string::npos) return true;
        // Keep the tail so a match spanning two chunks is still found.
        if (window.size() > needle.size()) {
            window.erase(0, window.size() - (needle.size() - 1));
        }
    }
    return false;
}

/// Walks the project's text assets, calling `visit(path, rel)` for each one
/// that mentions `needle`. `skip` is the referenced asset itself, which holds
/// its own id as identity rather than as a reference.
template <typename Fn>
void for_each_referencing_asset(const fs::path& root, const std::string& skip_relative,
                                const std::string& needle, const Fn& visit) {
    std::error_code ec;
    fs::recursive_directory_iterator it(root, ec);
    const fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        auto name = entry.path().filename().string();

        // Prune whole directories rather than filtering their files. Recursing
        // into build/ and .git/ costs more than everything else combined —
        // this project has 9200 files, 2800 of which are outside them.
        if (entry.is_directory()) {
            if (name.starts_with(".") || name == "build") {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!entry.is_regular_file()) continue;
        if (!is_text_asset(entry.path().extension().string())) continue;

        auto rel = fs::relative(entry.path(), root, ec).string();
        if (ec) {
            ec.clear();
            continue;
        }
        if (rel == skip_relative) continue;
        if (!file_contains(entry.path(), needle)) continue;

        visit(entry.path(), rel);
    }
}

} // namespace

std::vector<AssetReference> find_asset_references(const fs::path& project_root,
                                                  const AssetDatabase& db,
                                                  const AssetId& id) {
    std::vector<AssetReference> found;
    if (!id.valid()) return found;

    const auto* self = db.find(id);
    auto needle = id.to_string();

    for_each_referencing_asset(project_root, self ? self->relative_path : std::string{},
                               needle, [&](const fs::path& path, const std::string& rel) {
        // Only files that matched are read in full, for the exact count.
        found.push_back({rel, count_occurrences(read_file(path), needle)});
    });

    return found;
}

uint32_t repoint_asset_references(const fs::path& project_root,
                                  const AssetDatabase& db,
                                  const AssetId& from,
                                  const AssetId& to) {
    if (!from.valid() || !to.valid() || from == to) return 0;

    const auto* self = db.find(from);
    auto needle = from.to_string();
    auto replacement = to.to_string();
    uint32_t rewritten = 0;

    for_each_referencing_asset(project_root, self ? self->relative_path : std::string{},
                               needle, [&](const fs::path& path, const std::string& rel) {
        auto text = read_file(path);
        if (text.empty()) return;

        std::string out;
        out.reserve(text.size());
        size_t pos = 0;
        for (size_t hit = text.find(needle); hit != std::string::npos;
             hit = text.find(needle, pos)) {
            out.append(text, pos, hit - pos);
            out.append(replacement);
            pos = hit + needle.size();
        }
        out.append(text, pos, std::string::npos);

        // Atomic, so a failure part-way cannot leave a half-rewritten asset.
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            if (!f.is_open()) {
                FJELL_CORE_WARN("Could not rewrite references in '{}'", rel);
                return;
            }
            f << out;
            if (!f) {
                FJELL_CORE_WARN("Could not rewrite references in '{}'", rel);
                return;
            }
        }

        std::error_code ec;
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            FJELL_CORE_WARN("Could not replace '{}': {}", rel, ec.message());
            return;
        }
        ++rewritten;
    });

    return rewritten;
}

} // namespace fjell
