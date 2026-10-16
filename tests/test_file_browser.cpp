#include "kit_harness.hpp"

#include "ftk/app/file_browser.hpp"
#include "ftk/base/delegate.hpp"
#include "ftk/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using ftk::FileBrowser;
using ftk::test::KitHarness;
namespace core = ftk::ui::core;
namespace fs = std::filesystem;

namespace {

// A folder of the test's own: `kit` and `props` folders, a hidden one, and
// a model and a note beside them.
struct Folder {
    fs::path root = fs::temp_directory_path() / ("ftk_file_browser_" + std::to_string(ftk::platform::process_id()));

    Folder() {
        fs::remove_all(root);
        fs::create_directories(root / "kit" / "walls");
        fs::create_directories(root / "props");
        fs::create_directories(root / ".cache");
        std::ofstream(root / "crate.glb") << "glb";
        std::ofstream(root / "notes.txt") << "notes";
    }
    ~Folder() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;
};

// The browser drawn in the harness's window as its own window draws it,
// keeping what it chose and whether it asked to close.
struct Browser {
    Folder folder;
    KitHarness h{{760.0f, 520.0f}};
    FileBrowser browser;
    ftk::Connection selected;
    std::vector<std::string> chosen;
    bool closed{false};

    explicit Browser(FileBrowser::Mode mode = FileBrowser::Mode::select_directory,
                     std::vector<std::string> extensions = {}) {
        selected = browser.on_selected.bind([this](const std::string& path) { chosen.push_back(path); });
        browser.start("Output folder", mode, "Choose", {.extensions = std::move(extensions), .start = folder.root});
        h.set_ui([this] { closed = browser.draw() || closed; });
        h.step(2);
    }

    [[nodiscard]] std::vector<std::string> listed() {
        std::vector<std::string> names;
        for (const auto& node : h.ui().semantics()) {
            if (node.role == core::Role::ListItem) names.emplace_back(node.label);
        }
        return names;
    }
    [[nodiscard]] const core::SemanticNode* row(const std::string& name) const {
        const core::SemanticNode* node = h.find(name, core::Role::ListItem);
        REQUIRE(node != nullptr);
        return node;
    }
    [[nodiscard]] const core::SemanticNode* button(const std::string& label) const {
        return h.find(label, core::Role::Button);
    }
    void click(const core::SemanticNode* node) {
        REQUIRE(node != nullptr);
        h.click_at(node->rect.centre());
    }
};

} // namespace

TEST_CASE("The folder browser lists the folders, not files or hidden ones", "[file_browser]") {
    Browser b;
    CHECK(b.listed() == std::vector<std::string>{"kit", "props"});
}

TEST_CASE("Choose takes the folder shown while none is selected, else the one selected", "[file_browser]") {
    Browser b;
    b.click(b.button("Choose"));
    REQUIRE(b.chosen.size() == 1);
    CHECK(fs::path(b.chosen.back()) == fs::canonical(b.folder.root));
    CHECK(b.closed);

    b.click(b.row("props"));
    b.click(b.button("Choose"));
    REQUIRE(b.chosen.size() == 2);
    CHECK(fs::path(b.chosen.back()) == fs::canonical(b.folder.root) / "props");
}

TEST_CASE("A double-click goes into a folder", "[file_browser]") {
    Browser b;
    b.h.double_click_at(b.row("kit")->rect.centre());
    b.h.step();
    CHECK(b.listed() == std::vector<std::string>{"walls"});
    CHECK(b.chosen.empty());
    CHECK_FALSE(b.closed);
}

TEST_CASE("Cancel closes the browser and chooses nothing", "[file_browser]") {
    Browser b;
    b.click(b.button("Cancel"));
    CHECK(b.closed);
    CHECK(b.chosen.empty());
}

TEST_CASE("A file browser lists the files it asks for and chooses only a file", "[file_browser]") {
    Browser b(FileBrowser::Mode::select_file, {".glb"});
    CHECK(b.listed() == std::vector<std::string>{"kit", "props", "crate.glb"});
    b.click(b.button("Choose"));
    CHECK(b.chosen.empty());
    b.click(b.row("crate.glb"));
    b.click(b.button("Choose"));
    REQUIRE(b.chosen.size() == 1);
    CHECK(fs::path(b.chosen.back()) == fs::canonical(b.folder.root) / "crate.glb");
}

TEST_CASE("Sorting the list keeps the entry selected", "[file_browser]") {
    Browser b;
    b.click(b.row("props"));
    b.h.step();
    REQUIRE(b.row("props")->state.selected);
    // Name, again: Z to A, props now first.
    b.click(b.button("Name"));
    b.h.step();
    CHECK(b.listed() == std::vector<std::string>{"props", "kit"});
    CHECK(b.row("props")->state.selected);
    CHECK_FALSE(b.row("kit")->state.selected);
    b.click(b.button("Choose"));
    REQUIRE(b.chosen.size() == 1);
    CHECK(fs::path(b.chosen.back()) == fs::canonical(b.folder.root) / "props");
}

