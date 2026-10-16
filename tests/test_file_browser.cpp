#include "kit_harness.hpp"

#include "ftk/app/file_browser.hpp"
#include "ftk/base/delegate.hpp"
#include "ftk/platform/platform.hpp"
#include "ftk/ui/kit/icons.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
    // A menu's entry, drawn as a line that can be picked.
    [[nodiscard]] const core::SemanticNode* menu_item(const std::string& label) const {
        return h.find(label, core::Role::ListItem);
    }
    void click(const core::SemanticNode* node) {
        REQUIRE(node != nullptr);
        h.click_at(node->rect.centre());
    }
    // The top bar's New folder.
    void new_folder() { click(h.find(ftk::ui::icon::new_folder, core::Role::Button)); }
    // The search, the first field drawn.
    [[nodiscard]] const core::SemanticNode* search() {
        for (const auto& node : h.ui().semantics()) {
            if (node.role == core::Role::TextField) return &node;
        }
        return nullptr;
    }
    // The field a new folder's name is typed in: the field with the keyboard.
    [[nodiscard]] const core::SemanticNode* naming() {
        for (const auto& node : h.ui().semantics()) {
            if (node.role == core::Role::TextField && node.state.focused) return &node;
        }
        return nullptr;
    }
    [[nodiscard]] std::vector<std::string> on_disk() const {
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(folder.root)) {
            if (entry.is_directory() && !entry.path().filename().string().starts_with(".")) {
                names.push_back(entry.path().filename().string());
            }
        }
        std::ranges::sort(names);
        return names;
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

TEST_CASE("New folder makes new_folder where the browser is, selected, its name open", "[file_browser][new_folder]") {
    Browser b;
    b.new_folder();
    CHECK(b.on_disk() == std::vector<std::string>{"kit", "new_folder", "props"});
    CHECK(b.listed() == std::vector<std::string>{"kit", "new_folder", "props"});
    CHECK(b.row("new_folder")->state.selected);
    REQUIRE(b.naming() != nullptr);
    CHECK(b.naming()->value == "new_folder");

    // The name typed over the one selected; Enter names it and chooses nothing.
    b.h.type("stones");
    b.h.press(ftk::ui::Key::Enter);
    b.h.step();
    CHECK(b.on_disk() == std::vector<std::string>{"kit", "props", "stones"});
    CHECK(b.listed() == std::vector<std::string>{"kit", "props", "stones"});
    CHECK(b.row("stones")->state.selected);
    CHECK(b.chosen.empty());
    CHECK_FALSE(b.closed);
    CHECK(b.browser.problem().empty());

    // Choose takes it.
    b.click(b.button("Choose"));
    REQUIRE(b.chosen.size() == 1);
    CHECK(fs::path(b.chosen.back()) == fs::canonical(b.folder.root) / "stones");
}

TEST_CASE("Esc keeps new_folder and leaves the browser open", "[file_browser][new_folder]") {
    Browser b;
    b.new_folder();
    b.h.type("stones");
    b.h.press(ftk::ui::Key::Escape);
    b.h.step();
    CHECK(b.on_disk() == std::vector<std::string>{"kit", "new_folder", "props"});
    CHECK(b.row("new_folder")->state.selected);
    CHECK(b.naming() == nullptr);
    CHECK_FALSE(b.closed);
}

TEST_CASE("A new folder is numbered when new_folder is taken", "[file_browser][new_folder]") {
    Browser b;
    fs::create_directory(b.folder.root / "new_folder");
    std::ofstream(b.folder.root / "new_folder_1") << "a file of that name";
    b.new_folder();
    CHECK(b.on_disk() == std::vector<std::string>{"kit", "new_folder", "new_folder_2", "props"});
    REQUIRE(b.naming() != nullptr);
    CHECK(b.naming()->value == "new_folder_2");
}

TEST_CASE("A name taken, with a slash or a leading dot, isn't used and the bar says why", "[file_browser][new_folder]") {
    for (const char* name : {"props", "a/b", "a\\b", ".hidden"}) {
        CAPTURE(name);
        Browser b;
        b.new_folder();
        b.h.type(name);
        b.h.press(ftk::ui::Key::Enter);
        b.h.step();
        CHECK(b.on_disk() == std::vector<std::string>{"kit", "new_folder", "props"});
        CHECK_FALSE(b.browser.problem().empty());
        CHECK_FALSE(b.closed);
        // Until the next click.
        b.click(b.row("kit"));
        CHECK(b.browser.problem().empty());
    }
}

TEST_CASE("Ctrl Shift N makes a folder, but not while a field has the keyboard", "[file_browser][new_folder]") {
    Browser b;
    b.h.press(ftk::ui::Key::N, {.ctrl = true, .shift = true});
    b.h.step();
    CHECK(b.on_disk() == std::vector<std::string>{"kit", "new_folder", "props"});
    // Its name is open: the keys are the name's.
    b.h.press(ftk::ui::Key::N, {.ctrl = true, .shift = true});
    b.h.press(ftk::ui::Key::Escape);
    b.h.step();
    CHECK(b.on_disk() == std::vector<std::string>{"kit", "new_folder", "props"});
    // And the search's.
    b.click(b.search());
    b.h.press(ftk::ui::Key::N, {.ctrl = true, .shift = true});
    b.h.step();
    CHECK(b.on_disk() == std::vector<std::string>{"kit", "new_folder", "props"});
    // Ctrl N alone is not it.
    b.click(b.row("kit"));
    b.h.press(ftk::ui::Key::N, {.ctrl = true});
    b.h.step();
    CHECK(b.on_disk() == std::vector<std::string>{"kit", "new_folder", "props"});
}

TEST_CASE("A right click in the list offers New folder", "[file_browser][new_folder]") {
    Browser b;
    b.h.click_at(b.row("props")->rect.centre(), ftk::ui::MouseButton::Right);
    b.h.step();
    const core::SemanticNode* item = b.menu_item("New folder");
    REQUIRE(item != nullptr);
    b.click(item);
    b.h.step();
    CHECK(b.on_disk() == std::vector<std::string>{"kit", "new_folder", "props"});
    REQUIRE(b.naming() != nullptr);
}

TEST_CASE("New folder clears the search, so the folder made shows", "[file_browser][new_folder]") {
    Browser b;
    b.click(b.search());
    b.h.type("kit");
    b.h.step(2);
    REQUIRE(b.listed() == std::vector<std::string>{"kit"});
    b.new_folder();
    CHECK(b.listed() == std::vector<std::string>{"kit", "new_folder", "props"});
    CHECK(b.row("new_folder")->state.selected);
}

TEST_CASE("A click on another row while naming names the folder and keeps that row selected", "[file_browser][new_folder]") {
    Browser b;
    b.new_folder();
    b.h.type("stones");
    b.click(b.row("kit"));
    b.h.step();
    CHECK(b.on_disk() == std::vector<std::string>{"kit", "props", "stones"});
    CHECK(b.row("kit")->state.selected);
    CHECK_FALSE(b.row("stones")->state.selected);
}

TEST_CASE("A click in the name being typed keeps it open", "[file_browser][new_folder]") {
    Browser b;
    b.new_folder();
    const core::SemanticNode* field = b.naming();
    REQUIRE(field != nullptr);
    b.h.click_at(field->rect.centre());
    b.h.step();
    REQUIRE(b.naming() != nullptr);
    b.h.press(ftk::ui::Key::A, {.ctrl = true});
    b.h.type("stones");
    b.h.press(ftk::ui::Key::Enter);
    b.h.step();
    CHECK(b.on_disk() == std::vector<std::string>{"kit", "props", "stones"});
}

TEST_CASE("A new folder far down the list is scrolled into sight", "[file_browser][new_folder]") {
    Browser b;
    for (int i = 0; i < 60; ++i) fs::create_directory(b.folder.root / ("m" + std::to_string(100 + i)));
    b.click(b.row("kit"));
    b.h.step();
    b.new_folder();
    b.h.step(2);
    const core::SemanticNode* field = b.naming();
    REQUIRE(field != nullptr);
    const core::SemanticNode* choose = b.button("Choose");
    REQUIRE(choose != nullptr);
    // Above the bar, below the top of the window.
    CHECK(field->rect.min.y > 0.0f);
    CHECK(field->rect.max.y < choose->rect.min.y);
}

TEST_CASE("A folder that can't be made says why and leaves nothing", "[file_browser][new_folder]") {
    Browser b;
    const fs::path locked = b.folder.root / "kit";
    b.h.double_click_at(b.row("kit")->rect.centre());
    b.h.step();
    fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
    // Written to again as the test ends, however it ends, so it can be removed.
    struct Unlock {
        fs::path folder;
        ~Unlock() {
            std::error_code ec;
            fs::permissions(folder, fs::perms::owner_all, ec);
        }
    } unlock{locked};
    // Where writing is allowed anyway (an administrator), there is nothing to see.
    if (std::ofstream(locked / "probe").good()) SKIP("the folder can be written to however its permissions are set");
    b.new_folder();
    CHECK_FALSE(b.browser.problem().empty());
    CHECK(b.listed() == std::vector<std::string>{"walls"});
    CHECK(b.naming() == nullptr);
}

TEST_CASE("New folder while a name is typed names that folder first", "[file_browser][new_folder]") {
    Browser b;
    b.new_folder();
    b.h.type("stones");
    b.new_folder();
    b.h.step();
    CHECK(b.on_disk() == std::vector<std::string>{"kit", "new_folder", "props", "stones"});
    REQUIRE(b.naming() != nullptr);
    CHECK(b.naming()->value == "new_folder");
    CHECK(b.row("new_folder")->state.selected);
}


