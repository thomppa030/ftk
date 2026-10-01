#include "core/base_path.hpp"
#include "core/engine_dir.hpp"
#include "core/project.hpp"
#include "ftk/base/log.hpp"

#include <stdexcept>

namespace fjell::base_path {

static std::filesystem::path compute_dir() {
    auto dir = find_engine_dir();
    if (dir.empty()) {
        throw std::runtime_error("Could not locate shaders/ directory from executable path");
    }
    FJELL_CORE_INFO("Base path: {}", dir.string());
    return dir;
}

const std::filesystem::path& dir() {
    static const auto d = compute_dir();
    return d;
}

std::string resolve(const std::string& relative) {
    return (dir() / relative).string();
}

const std::filesystem::path& sdk_include() {
    static const auto path = [] {
        // Installed layout: headers land in sdk/fjell, so sdk/ is the include root.
        auto installed = dir() / "sdk";
        if (std::filesystem::is_directory(installed)) return installed;
        // Dev layout: included straight out of the engine source tree.
        return dir() / ".." / "src" / "scripting";
    }();
    return path;
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

bool references_asset(const std::string& reference, const std::string& extension) {
    if (reference.empty()) return false;
    if (is_asset_id(reference)) return true;
    return reference.ends_with(extension);
}

std::string asset_reference(const std::string& path_or_id, const Project* project) {
    if (path_or_id.empty() || is_asset_id(path_or_id)) return path_or_id;
    if (!project) return path_or_id;

    auto id = project->id_of(path_or_id);
    return id.valid() ? id.to_string() : path_or_id;
}

std::string asset_display_path(const std::string& reference, const Project* project) {
    if (reference.empty() || !is_asset_id(reference)) return reference;
    if (!project) return {};
    return project->path_of(AssetId::from_string(reference));
}

std::string resolve_shared(const std::string& reference, const Project* project) {
    // An id is always the project's — the engine's own files are not in its
    // database — so that case is resolve_asset's already.
    if (is_asset_id(reference)) {
        return resolve_asset(reference, project);
    }

    if (project) {
        auto in_project = project->resolve(reference);
        if (!in_project.empty() && std::filesystem::exists(in_project)) {
            return in_project;
        }
    }

    auto in_engine = resolve(reference);
    if (std::filesystem::exists(in_engine)) {
        return in_engine;
    }
    return {};
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
