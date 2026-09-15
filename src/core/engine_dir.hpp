#pragma once

#include "platform/platform.hpp"

#include <filesystem>

namespace fjell {

/// The directory the engine's own files live in — shaders/, engine_assets/,
/// assets/ — found by walking up from the executable. Covers the build
/// layout (build/src/fjell → build/) and the installed one (Fjell/fjell →
/// Fjell/). Empty when neither matches, which every caller treats as a
/// broken install rather than something to work around.
[[nodiscard]] inline std::filesystem::path find_engine_dir() {
    auto dir = platform::executable_path().parent_path();
    for (int i = 0; i < 3; ++i) {
        if (std::filesystem::is_directory(dir / "shaders")) return dir;
        dir = dir.parent_path();
    }
    return {};
}

} // namespace fjell
