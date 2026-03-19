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

std::string resolve_asset(const std::string& relative, const Project* project) {
    if (project) {
        return project->resolve(relative);
    }
    return resolve(relative);
}

} // namespace fjell::base_path
