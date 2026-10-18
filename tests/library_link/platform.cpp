// Links ftk-platform alone and whole (tests/CMakeLists.txt), so it builds
// only if everything in the library finds what it needs in the library and
// its own dependencies. Running it calls into each part once.

#include "ftk/base/log.hpp"
#include "ftk/platform/platform.hpp"
#include "ftk/platform/shared_library.hpp"
#include "ftk/platform/tree_watcher.hpp"

#include <filesystem>

int main() {
    ftk::log::init({.level = spdlog::level::warn});

    const std::filesystem::path exe = ftk::platform::executable_path();
    const auto pid = ftk::platform::process_id();

    ftk::platform::TreeWatcher watcher;
    const bool watching = watcher.watch(exe.parent_path());
    (void)watcher.poll();

    ftk::platform::SharedLibrary library;
    const bool loaded = library.load(exe.parent_path() / "no-such-library");

    const bool folders = ftk::platform::config_dir().is_absolute() && ftk::platform::state_dir().is_absolute() &&
                         ftk::platform::config_dir() != ftk::platform::state_dir() &&
                         ftk::platform::cache_dir().is_absolute() && ftk::platform::documents_dir().is_absolute() &&
                         ftk::platform::pictures_dir().is_absolute();

    ftk::log::shutdown();
    const bool ran = exe.is_absolute() && pid != 0 && watching && !loaded && folders;
    return ran ? 0 : 1;
}
