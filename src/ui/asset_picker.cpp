#include "ui/asset_picker.hpp"
#include "ui/asset_thumbnail_renderer.hpp"
#include "ui/grouped_picker.hpp"
#include "core/asset_type_registry.hpp"
#include "core/base_path.hpp"
#include "core/project.hpp"
#include "core/thread_pool.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fjell::asset_picker {
namespace {

constexpr float POPUP_WIDTH = 320.0F;
constexpr float POPUP_HEIGHT = 400.0F;

// A picker shows what it has and fills the rest in over the next frames: the
// thumbnail renderer draws one asset per frame, so asking for a whole folder
// at once only grows a queue the user is scrolling past anyway.
constexpr int MAX_REQUESTS_PER_FRAME = 8;

/// Where an asset came from. Only these two: an asset is under the project or
/// it ships with the engine, and the reference written for it differs.
enum class Source { project, engine };

struct Entry {
    std::string relative;     // as written into the field
    std::string absolute;     // for the thumbnail
    std::string filename;     // what the row shows
    std::string extension;
    Source source{Source::project};
};

/// One listing, keyed by the extensions that produced it. Two fields taking
/// the same types share it rather than walking the project twice.
struct Listing {
    std::vector<std::string> extensions;
    std::vector<Entry> entries;
    bool valid{false};
};

Listing g_listing;

ThreadPool* g_pool{nullptr};
// The walk in flight, if any, and the extensions it was started for. Only the
// main thread touches these: the worker's only contact with the picker is the
// vector it returns through the future.
std::future<std::vector<Entry>> g_scan;
std::vector<std::string> g_scan_extensions;
bool g_scanning{false};

bool matches(const std::filesystem::path& path,
             std::span<const std::string> extensions) {
    if (extensions.empty()) return true;
    auto ext = path.extension().string();
    return std::ranges::any_of(extensions,
                               [&](const std::string& e) { return ext == e; });
}

/// Walk one root and add every accepted file under it. `.fjcache` and the
/// project's own `.fjell` bookkeeping hold generated files, never assets.
void collect(const std::filesystem::path& root, Source source,
             std::span<const std::string> extensions,
             std::vector<Entry>& out) {
    if (root.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return;

    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        const auto& path = it->path();
        auto name = path.filename().string();
        if (!name.empty() && name.front() == '.') {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (!matches(path, extensions)) continue;

        Entry entry;
        entry.relative = std::filesystem::relative(path, root, ec).generic_string();
        if (ec) continue;
        entry.absolute = path.string();
        entry.filename = name;
        entry.extension = path.extension().string();
        entry.source = source;
        out.push_back(std::move(entry));
    }
}

/// Walk both roots. Runs on a worker: it touches nothing but its arguments
/// and the vector it returns.
std::vector<Entry> scan(std::vector<std::string> extensions,
                        std::filesystem::path project_root,
                        std::filesystem::path engine_root) {
    std::vector<Entry> entries;
    const bool has_project = !project_root.empty();

    if (has_project) {
        collect(project_root, Source::project, extensions, entries);
    }
    collect(engine_root, Source::engine, extensions, entries);

    // An engine asset the project also carries would be listed twice, and the
    // project's copy is the one a reference resolves to.
    if (has_project) {
        std::erase_if(entries, [&](const Entry& e) {
            return e.source == Source::engine &&
                   std::ranges::any_of(entries, [&](const Entry& o) {
                       return o.source == Source::project && o.relative == e.relative;
                   });
        });
    }

    std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
        if (a.source != b.source) return a.source < b.source;
        if (a.extension != b.extension) return a.extension < b.extension;
        return a.relative < b.relative;
    });
    return entries;
}

/// Start a walk for these extensions, on the pool when there is one. The
/// picker draws what it has until the result lands.
void begin_scan(std::span<const std::string> extensions, const Project* project) {
    std::vector<std::string> exts(extensions.begin(), extensions.end());
    std::filesystem::path project_root = project != nullptr ? project->root() : std::filesystem::path{};
    auto engine_root = base_path::dir();

    g_scan_extensions = exts;

    if (g_pool == nullptr) {
        // No pool yet (a tool or a test drawing the widget): the walk still
        // has to happen, it just happens here.
        g_listing.entries = scan(std::move(exts), std::move(project_root), std::move(engine_root));
        g_listing.extensions = g_scan_extensions;
        g_listing.valid = true;
        return;
    }

    g_scan = g_pool->submit([exts = std::move(exts),
                             project_root = std::move(project_root),
                             engine_root = std::move(engine_root)]() mutable {
        return scan(std::move(exts), std::move(project_root), std::move(engine_root));
    });
    g_scanning = true;
}

/// Take the walk's result once it is ready. Never blocks: a picker open over
/// a slow directory keeps drawing until then.
void poll_scan() {
    if (!g_scanning) return;
    if (!g_scan.valid()) {
        g_scanning = false;
        return;
    }
    if (g_scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    g_listing.entries = g_scan.get();
    g_listing.extensions = g_scan_extensions;
    g_listing.valid = true;
    g_scanning = false;
}

bool listing_covers(std::span<const std::string> extensions) {
    return g_listing.valid &&
           std::ranges::equal(g_listing.extensions, extensions);
}

/// Whether a walk for these extensions is already on its way, so opening a
/// second picker on the same types does not start another.
bool scan_covers(std::span<const std::string> extensions) {
    return g_scanning && std::ranges::equal(g_scan_extensions, extensions);
}

/// The label the type registry gives this extension, for the type header.
/// An extension no type claims reads as itself.
std::string type_label(const std::string& extension) {
    std::string label;
    AssetTypeRegistry::instance().for_each([&](const AssetTypeEntry& e) {
        if (label.empty() && e.extension == extension) label = e.label;
    });
    if (label.empty()) return extension;
    return label + " (" + extension + ")";
}

/// The thumbnail for an asset, asking for one when it has none yet. Requests
/// are rationed: the renderer draws one asset per frame, so asking for a whole
/// folder at once only grows a queue the user is scrolling past anyway.
ImTextureID thumbnail_for(const Entry& entry, int& requests) {
    auto* thumbnails = AssetThumbnailRenderer::instance();
    if (thumbnails == nullptr || !thumbnails->supports(entry.absolute)) return 0;

    VkDescriptorSet desc = thumbnails->descriptor(entry.absolute);
    if (desc == VK_NULL_HANDLE) {
        if (requests < MAX_REQUESTS_PER_FRAME) {
            thumbnails->request(entry.absolute);
            ++requests;
        }
        return 0;
    }
    return reinterpret_cast<ImTextureID>(desc);
}

/// Turn the listing into the picker's groups: one per source, or one per
/// source and type when the field takes more than one type.
std::vector<grouped_picker::Group> build_groups(std::span<const std::string> extensions) {
    // Only one accepted type means a type header would say what the field
    // already says.
    const bool split_by_type = extensions.size() != 1;

    std::vector<grouped_picker::Group> groups;
    int requests = 0;

    // The entries are sorted by source then extension, so a group starts
    // wherever either changes.
    std::optional<Source> current_source;
    std::string current_extension;

    for (const auto& entry : g_listing.entries) {
        const bool new_source = !current_source || *current_source != entry.source;
        const bool new_type = split_by_type && current_extension != entry.extension;
        if (new_source || new_type) {
            current_source = entry.source;
            current_extension = entry.extension;

            std::string label = entry.source == Source::project ? "PROJECT" : "ENGINE";
            if (split_by_type) label += "  •  " + type_label(entry.extension);
            groups.push_back({.label = std::move(label), .items = {}, .open = true});
        }

        auto folder = std::filesystem::path(entry.relative).parent_path().generic_string();
        groups.back().items.push_back({
            .value = entry.relative,
            .label = entry.filename,
            .sublabel = folder.empty() ? "/" : folder,
            .preview = thumbnail_for(entry, requests),
            .tag = {},
            .tag_color = 0,
            // The row shows the filename and its folder; searching the whole
            // relative path is what makes "materials/brick" find it.
            .search_text = entry.relative,
        });
    }
    return groups;
}

} // namespace

void set_thread_pool(ThreadPool* pool) {
    g_pool = pool;
}

void open(const char* widget_id) {
    grouped_picker::open(widget_id);
}

void invalidate() {
    g_listing.valid = false;
    g_listing.entries.clear();
    g_listing.extensions.clear();
    // A walk started before the change would land with the old contents and
    // look like a fresh listing. Waiting on it here is what keeps the worker
    // from outliving the pool at shutdown; it only reads the filesystem, so
    // there is nothing for it to block on.
    if (g_scan.valid()) g_scan.wait();
    g_scan = {};
    g_scanning = false;
    g_scan_extensions.clear();
}

bool draw(const char* widget_id, std::span<const std::string> extensions,
          const Project* project, std::string& picked) {
    if (!grouped_picker::is_open(widget_id)) return false;

    poll_scan();

    // The listing is taken when a picker opens, not per frame: a field is
    // clicked far less often than it is drawn, and the walk is what keeps the
    // list honest about assets added since. A listing already covering these
    // types is reused, so reopening a picker is free.
    if (!listing_covers(extensions) && !scan_covers(extensions)) {
        begin_scan(extensions, project);
    }

    const grouped_picker::Config config{
        .search_hint = "Search assets",
        .empty_message = "No matching assets in this project.",
        .loading_message = "Looking for assets...",
        .loading = g_scanning,
        .hide_group_headers = false,
        .size = {POPUP_WIDTH, POPUP_HEIGHT},
    };

    return grouped_picker::draw(widget_id, build_groups(extensions), config, picked);
}

} // namespace fjell::asset_picker
