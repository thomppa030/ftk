#include "ftk/platform/platform.hpp"
#include "ftk/platform/tree_watcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace ftk::platform;
namespace fs = std::filesystem;
using Kind = TreeEvent::Kind;

namespace {

// A folder of its own for each test, gone after it.
struct Folder {
    fs::path base;
    explicit Folder(const char* name) {
        base = fs::temp_directory_path() / ("ftk_tree_watcher_" + std::to_string(process_id())) / name;
        fs::remove_all(base);
        fs::create_directories(base);
    }
    ~Folder() { fs::remove_all(base); }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;
};

void write(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }

// What the watcher tells once `wanted` events have come, or two seconds
// have passed: Windows reports a little after a change, Linux at once. A
// short wait after the last catches anything still to come.
std::vector<TreeEvent> settle(TreeWatcher& watcher, size_t wanted) {
    using namespace std::chrono_literals;
    std::vector<TreeEvent> all;
    const auto take = [&] {
        auto events = watcher.poll();
        all.insert(all.end(), events.begin(), events.end());
        all = tidy_tree_events(std::move(all));
    };
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (all.size() < wanted && std::chrono::steady_clock::now() < until) {
        take();
        std::this_thread::sleep_for(10ms);
    }
    std::this_thread::sleep_for(wanted == 0 ? 200ms : 50ms);
    take();
    return all;
}

TreeEvent added(const fs::path& path, bool folder = false) {
    return {.kind = Kind::added, .path = path, .folder = folder};
}
TreeEvent changed(const fs::path& path) { return {.kind = Kind::changed, .path = path}; }
TreeEvent removed(const fs::path& path) { return {.kind = Kind::removed, .path = path}; }
TreeEvent renamed(const fs::path& from, const fs::path& to, bool folder = false) {
    return {.kind = Kind::renamed, .path = to, .from = from, .folder = folder};
}

} // namespace

TEST_CASE("A poll's events are tidied to what a reader needs", "[platform][tree_watcher]") {
    CHECK(tidy_tree_events({added("a"), changed("a"), changed("a")}) == std::vector{added("a")});
    CHECK(tidy_tree_events({changed("a"), changed("a")}) == std::vector{changed("a")});
    CHECK(tidy_tree_events({added("a"), removed("a")}).empty());
    CHECK(tidy_tree_events({removed("a"), added("a")}) == std::vector{changed("a")});
    CHECK(tidy_tree_events({removed("a"), added("a"), removed("a")}) == std::vector{removed("a")});
    CHECK(tidy_tree_events({added("a.tmp"), changed("a.tmp"), renamed("a.tmp", "a")}) == std::vector{added("a")});
    CHECK(tidy_tree_events({renamed("a", "b"), removed("b")}) == std::vector{removed("a")});
    CHECK(tidy_tree_events({added("a"), changed("b"), removed("c")}) ==
          std::vector{added("a"), changed("b"), removed("c")});
}

TEST_CASE("A file made in the tree is added once, however it was written", "[platform][tree_watcher]") {
    const Folder folder("made");
    TreeWatcher watcher;
    REQUIRE(watcher.watch(folder.base));
    write(folder.base / "a.png", "image");
    CHECK(settle(watcher, 1) == std::vector{added(folder.base / "a.png")});
}

TEST_CASE("A file written is changed, and one removed is removed", "[platform][tree_watcher]") {
    const Folder folder("written");
    write(folder.base / "a.png", "image");
    TreeWatcher watcher;
    REQUIRE(watcher.watch(folder.base));

    write(folder.base / "a.png", "another image");
    CHECK(settle(watcher, 1) == std::vector{changed(folder.base / "a.png")});
    fs::remove(folder.base / "a.png");
    CHECK(settle(watcher, 1) == std::vector{removed(folder.base / "a.png")});
}

TEST_CASE("A file renamed within the tree is one rename, across folders too", "[platform][tree_watcher]") {
    const Folder folder("renamed");
    write(folder.base / "a.png", "image");
    fs::create_directories(folder.base / "sub");
    TreeWatcher watcher;
    REQUIRE(watcher.watch(folder.base));

    fs::rename(folder.base / "a.png", folder.base / "sub" / "b.png");
    CHECK(settle(watcher, 1) == std::vector{renamed(folder.base / "a.png", folder.base / "sub" / "b.png")});
}

TEST_CASE("A file moved out of the tree is removed, and one moved in is added", "[platform][tree_watcher]") {
    const Folder folder("moved");
    const Folder outside("outside");
    write(folder.base / "a.png", "image");
    write(outside.base / "c.png", "image");
    TreeWatcher watcher;
    REQUIRE(watcher.watch(folder.base));

    fs::rename(folder.base / "a.png", outside.base / "a.png");
    CHECK(settle(watcher, 1) == std::vector{removed(folder.base / "a.png")});
    fs::rename(outside.base / "c.png", folder.base / "c.png");
    CHECK(settle(watcher, 1) == std::vector{added(folder.base / "c.png")});
}

TEST_CASE("A folder that appears with files in it is watched at once", "[platform][tree_watcher]") {
    const Folder folder("new_folder");
    TreeWatcher watcher;
    REQUIRE(watcher.watch(folder.base));

    fs::create_directories(folder.base / "d");
    write(folder.base / "d" / "x.png", "image");
    const auto made = settle(watcher, 2);
    CHECK(made == std::vector{added(folder.base / "d", true), added(folder.base / "d" / "x.png")});

    write(folder.base / "d" / "x.png", "another image");
    CHECK(settle(watcher, 1) == std::vector{changed(folder.base / "d" / "x.png")});
}

TEST_CASE("A folder renamed is still watched under its new name", "[platform][tree_watcher]") {
    const Folder folder("renamed_folder");
    fs::create_directories(folder.base / "d");
    write(folder.base / "d" / "x.png", "image");
    TreeWatcher watcher;
    REQUIRE(watcher.watch(folder.base));

    fs::rename(folder.base / "d", folder.base / "e");
    CHECK(settle(watcher, 1) == std::vector{renamed(folder.base / "d", folder.base / "e", true)});
    write(folder.base / "e" / "x.png", "another image");
    CHECK(settle(watcher, 1) == std::vector{changed(folder.base / "e" / "x.png")});
}

TEST_CASE("Folders the watcher skips are left alone, with all under them", "[platform][tree_watcher]") {
    const Folder folder("skipped");
    fs::create_directories(folder.base / "build" / "deep");
    TreeWatcher watcher;
    REQUIRE(watcher.watch(folder.base, [](std::string_view name) { return name == "build" || name.starts_with('.'); }));

    write(folder.base / "build" / "x.png", "image");
    write(folder.base / "build" / "deep" / "y.png", "image");
    fs::create_directories(folder.base / ".cache");
    write(folder.base / ".cache" / "z.png", "image");
    CHECK(settle(watcher, 0).empty());
}

TEST_CASE("A file moved into a skipped folder is removed, a folder moved out of one added", "[platform][tree_watcher]") {
    const Folder folder("skipped_moves");
    fs::create_directories(folder.base / "build" / "d");
    write(folder.base / "build" / "d" / "x.png", "image");
    write(folder.base / "a.png", "image");
    TreeWatcher watcher;
    REQUIRE(watcher.watch(folder.base, [](std::string_view name) { return name == "build" || name.starts_with('.'); }));

    fs::rename(folder.base / "a.png", folder.base / "build" / "a.png");
    CHECK(settle(watcher, 1) == std::vector{removed(folder.base / "a.png")});
    fs::rename(folder.base / "build" / "d", folder.base / "d");
    CHECK(settle(watcher, 2) == std::vector{added(folder.base / "d", true), added(folder.base / "d" / "x.png")});
}

TEST_CASE("A file saved by renaming a written copy over it arrives once", "[platform][tree_watcher]") {
    const Folder folder("saved");
    write(folder.base / "a.png", "image");
    TreeWatcher watcher;
    REQUIRE(watcher.watch(folder.base));

    write(folder.base / "a.png.tmp", "another image");
    fs::rename(folder.base / "a.png.tmp", folder.base / "a.png");
    // Linux tells nothing of the file replaced, so it reads as added; a
    // system that tells of it gives changed. Either says: read it again.
    const auto events = settle(watcher, 1);
    REQUIRE(events.size() == 1);
    CHECK(events[0].path == folder.base / "a.png");
    CHECK((events[0].kind == Kind::added || events[0].kind == Kind::changed));
}
