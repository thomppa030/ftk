// Links ftk-platform alone and whole (tests/CMakeLists.txt), so it builds
// only if everything in the library finds what it needs in the library and
// its own dependencies. Running it calls into each part once.

#include "ftk/base/log.hpp"
#include "ftk/platform/file_watcher.hpp"
#include "ftk/platform/platform.hpp"
#include "ftk/platform/shared_library.hpp"

#include <filesystem>

int main() {
    fjell::log::init({.level = spdlog::level::warn});

    const std::filesystem::path exe = fjell::platform::executable_path();
    const auto pid = fjell::platform::process_id();

    fjell::platform::FileWatcher watcher;
    const bool watching = watcher.watch(exe.parent_path(), [](const std::filesystem::path&) {});
    (void)watcher.poll();

    fjell::platform::SharedLibrary library;
    const bool loaded = library.load(exe.parent_path() / "no-such-library");

    fjell::log::shutdown();
    const bool ran = exe.is_absolute() && pid != 0 && watching && !loaded;
    return ran ? 0 : 1;
}
