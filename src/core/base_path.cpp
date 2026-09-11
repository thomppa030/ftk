#include "core/base_path.hpp"
#include "core/project.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"

#include <stdexcept>

namespace fjell::base_path {

static std::filesystem::path compute_dir() {
    auto exe = platform::executable_path();

    // Walk up from the executable directory until we find "shaders/"
    // This handles both the dev layout (build/src/fjell → build/)
    // and the install layout (Fjell/fjell → Fjell/)
    auto dir = exe.parent_path();
    for (int i = 0; i < 3; ++i) {
        if (std::filesystem::is_directory(dir / "shaders")) {
            FJELL_CORE_INFO("Base path: {}", dir.string());
            return dir;
        }
        dir = dir.parent_path();
    }

    throw std::runtime_error("Could not locate shaders/ directory from executable path");
}

const std::filesystem::path& dir() {
    static const auto d = compute_dir();
    return d;
}

std::string resolve(const std::string& relative) {
    return (dir() / relative).string();
}

bool is_asset_id(const std::string& reference) {
    // An id is 32 hex characters. No path the engine stores looks like that —
    // they all carry a directory separator or an extension.
    if (reference.size() != 32) return false;
    for (char c : reference) {
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

std::string asset_reference(const std::string& path_or_id, const Project* project) {
    if (path_or_id.empty() || is_asset_id(path_or_id)) return path_or_id;
    if (!project) return path_or_id;

    auto id = project->id_of(path_or_id);
    return id.valid() ? id.to_string() : path_or_id;
}

std::string resolve_asset(const std::string& reference, const Project* project) {
    if (is_asset_id(reference)) {
        if (!project) {
            FJELL_CORE_WARN("Asset id '{}' referenced with no project open", reference);
            return {};
        }
        auto path = project->resolve(AssetId::from_string(reference));
        if (path.empty()) {
            FJELL_CORE_WARN("Asset id '{}' is not in the asset database", reference);
        }
        return path;
    }

    if (project) {
        return project->resolve(reference);
    }
    return resolve(reference);
}

} // namespace fjell::base_path
