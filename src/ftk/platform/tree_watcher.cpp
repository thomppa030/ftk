#include "ftk/platform/tree_watcher.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace ftk::platform {

std::vector<TreeEvent> tidy_tree_events(std::vector<TreeEvent> events) {
    using Kind = TreeEvent::Kind;
    // Each event kept, or nothing where a later one took its place, and
    // where the last one kept for each path is.
    std::vector<std::optional<TreeEvent>> kept;
    std::unordered_map<std::string, size_t> last;
    const auto key = [](const std::filesystem::path& path) { return path.lexically_normal().generic_string(); };
    const auto last_kind = [&](const std::filesystem::path& path) -> std::optional<Kind> {
        const auto found = last.find(key(path));
        if (found == last.end() || !kept[found->second]) return std::nullopt;
        return kept[found->second]->kind;
    };
    const auto forget = [&](const std::filesystem::path& path) {
        const auto found = last.find(key(path));
        if (found == last.end()) return;
        kept[found->second].reset();
        last.erase(found);
    };
    const auto keep = [&](TreeEvent event) {
        last[key(event.path)] = kept.size();
        kept.emplace_back(std::move(event));
    };
    // A file made at `path`: new, or in place of one gone or written this
    // poll, which is a change to what was there.
    const auto arrive = [&](std::filesystem::path path, bool folder) {
        const std::optional<Kind> before = last_kind(path);
        if (before == Kind::added) return;
        forget(path);
        const bool replaced = before == Kind::removed || before == Kind::changed;
        keep({.kind = replaced ? Kind::changed : Kind::added, .path = std::move(path), .folder = folder});
    };

    for (TreeEvent& event : events) {
        const std::optional<Kind> before = last_kind(event.path);
        switch (event.kind) {
            case Kind::added:
                arrive(std::move(event.path), event.folder);
                break;
            case Kind::changed:
                if (before == Kind::added || before == Kind::changed) break;
                keep(std::move(event));
                break;
            case Kind::removed:
                // A file made this poll and gone again was never there; one
                // renamed and gone is gone from where it was.
                if (before == Kind::added) {
                    forget(event.path);
                    break;
                }
                if (before == Kind::renamed) {
                    std::filesystem::path from = kept[last[key(event.path)]]->from;
                    forget(event.path);
                    keep({.kind = Kind::removed, .path = std::move(from), .folder = event.folder});
                    break;
                }
                forget(event.path);
                keep(std::move(event));
                break;
            case Kind::renamed:
                // One made this poll and renamed arrives where it went, as a
                // file saved by renaming a written copy over it does.
                if (last_kind(event.from) == Kind::added) {
                    forget(event.from);
                    arrive(std::move(event.path), event.folder);
                    break;
                }
                forget(event.path);
                keep(std::move(event));
                break;
            case Kind::lost:
                keep(std::move(event));
                break;
        }
    }

    std::vector<TreeEvent> out;
    out.reserve(kept.size());
    for (auto& event : kept) {
        if (event) out.push_back(std::move(*event));
    }
    return out;
}

} // namespace ftk::platform
