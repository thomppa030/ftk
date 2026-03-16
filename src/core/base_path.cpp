#include "core/base_path.hpp"
#include "core/project.hpp"
#include "core/log.hpp"

#include <unistd.h>

#include <array>
#include <stdexcept>

namespace fjell::base_path {

static std::filesystem::path compute_dir() {
    std::array<char, 4096> buf{};
    auto len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len <= 0) {
        throw std::runtime_error("Failed to read /proc/self/exe");
    }
    buf[len] = '\0';

    // Walk up from the executable directory until we find "shaders/"
    // This handles both the dev layout (build/src/fjell → build/)
    // and the install layout (Fjell/fjell → Fjell/)
    auto dir = std::filesystem::path(buf.data()).parent_path();
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
