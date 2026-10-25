#include "ftk/app/desktop_entry.hpp"
#include "ftk/base/file_io.hpp"
#include "ftk/image/png.hpp"
#include "ftk/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* TILE = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect x="2" y="2" width="96" height="96" rx="22" fill="#1e1f22"/></svg>)";

// A data folder of the test's own, emptied, with the tile written beside it.
struct Folder {
    fs::path root = fs::temp_directory_path() / ("ftk_desktop_entry_" + std::to_string(ftk::platform::process_id()));
    fs::path data = root / "share";
    fs::path icon = root / "tile.svg";

    Folder() {
        fs::remove_all(root);
        fs::create_directories(root);
        // As its bytes: the entry keeps the icon as it reads, and a text
        // stream on Windows would write the tile's newline as "\r\n".
        std::ofstream(icon, std::ios::binary) << TILE;
    }
    ~Folder() { fs::remove_all(root); }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;
    Folder(Folder&&) = delete;
    Folder& operator=(Folder&&) = delete;
};

bool has_line(const std::string& text, const std::string& line) {
    return text.find("\n" + line + "\n") != std::string::npos;
}

} // namespace

TEST_CASE("A desktop entry names its program, its icon and its id", "[app][desktop_entry]") {
    const Folder folder;
    const ftk::DesktopEntry entry{.id = "fjedit",
                                  .name = "fjedit",
                                  .comment = "Code editor",
                                  .categories = {"Development", "TextEditor"},
                                  .program = "/opt/fjedit/fjedit",
                                  .icon = folder.icon};
    const auto written = ftk::create_desktop_entry(entry, folder.data);
    REQUIRE(written.has_value());
    CHECK(*written == folder.data / "applications" / "fjedit.desktop");

    const auto text = ftk::read_text(*written);
    REQUIRE(text.has_value());
    CHECK(text->starts_with("[Desktop Entry]\n"));
    CHECK(has_line(*text, "Type=Application"));
    CHECK(has_line(*text, "Name=fjedit"));
    CHECK(has_line(*text, "Comment=Code editor"));
    CHECK(has_line(*text, "Exec=/opt/fjedit/fjedit"));
    CHECK(has_line(*text, "TryExec=/opt/fjedit/fjedit"));
    CHECK(has_line(*text, "Icon=fjedit"));
    CHECK(has_line(*text, "Categories=Development;TextEditor;"));
    CHECK(has_line(*text, "StartupWMClass=fjedit"));
}

TEST_CASE("A desktop entry's icon is the SVG and a PNG at every theme size", "[app][desktop_entry]") {
    const Folder folder;
    REQUIRE(ftk::create_desktop_entry({.id = "fjink", .name = "fjink", .program = "/opt/fjink", .icon = folder.icon},
                                      folder.data)
                .has_value());
    const fs::path icons = folder.data / "icons" / "hicolor";
    const auto kept = ftk::read_text(icons / "scalable" / "apps" / "fjink.svg");
    REQUIRE(kept.has_value());
    CHECK(*kept == TILE);
    for (const uint32_t size : ftk::DESKTOP_ICON_SIZES) {
        const std::string sized = std::to_string(size) + "x" + std::to_string(size);
        const auto bytes = ftk::read_text(icons / sized / "apps" / "fjink.png");
        REQUIRE(bytes.has_value());
        const auto png = ftk::image::decode_png(std::as_bytes(std::span(*bytes)));
        REQUIRE(png.has_value());
        CHECK(png->width == size);
        CHECK(png->height == size);
    }
}

TEST_CASE("A desktop entry tells which program it opens, however its path is spelled", "[app][desktop_entry]") {
    const Folder folder;
    for (const fs::path& program : {fs::path("/home/kay/Fjell/build/src/fjell-hub"),
                                   fs::path("/home/kay/my games/fjell 1.0/fjell-hub"),
                                   fs::path(R"(/srv/$HOME/back\slash/"quoted"/100%/fjell-hub)")}) {
        REQUIRE(ftk::create_desktop_entry({.id = "fjell-hub", .name = "fjell", .program = program, .icon = folder.icon},
                                          folder.data)
                    .has_value());
        CHECK(ftk::desktop_entry_program("fjell-hub", folder.data) == program);
    }
}

TEST_CASE("An Exec line quotes a path with spaces and escapes what the spec reserves", "[app][desktop_entry]") {
    CHECK(ftk::desktop_exec("/usr/bin/fjedit") == "/usr/bin/fjedit");
    CHECK(ftk::desktop_exec("/home/kay/my games/fjell") == "\"/home/kay/my games/fjell\"");
    CHECK(ftk::desktop_exec("/a/100%/b") == "/a/100%%/b");
    // A quote is escaped inside the quotes, and that backslash again as a string's.
    CHECK(ftk::desktop_exec("/a\"b") == "\"/a\\\\\"b\"");
}

TEST_CASE("A desktop entry written by hand is read for its program", "[app][desktop_entry]") {
    const Folder folder;
    fs::create_directories(folder.data / "applications");
    std::ofstream(folder.data / "applications" / "fjell-hub.desktop")
        << "[Desktop Entry]\nName=Fjell Hub\nExec=/home/kay/Fjell/build/src/fjell-hub\nIcon=/x.png\n"
           "[Desktop Action other]\nExec=/somewhere/else\n";
    CHECK(ftk::desktop_entry_program("fjell-hub", folder.data) == fs::path("/home/kay/Fjell/build/src/fjell-hub"));
    CHECK_FALSE(ftk::desktop_entry_program("fjedit", folder.data).has_value());
}

TEST_CASE("What can't make a desktop entry says why", "[app][desktop_entry]") {
    const Folder folder;
    std::ofstream(folder.root / "list.svg") << "a shopping list";
    std::ofstream(folder.root / "wide.svg") << R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 10"/>)";

    const auto not_svg = ftk::create_desktop_entry({.id = "a", .program = "/a", .icon = folder.root / "list.svg"},
                                                   folder.data);
    REQUIRE_FALSE(not_svg.has_value());
    CHECK_FALSE(not_svg.error().empty());

    const auto wide = ftk::create_desktop_entry({.id = "a", .program = "/a", .icon = folder.root / "wide.svg"},
                                                folder.data);
    REQUIRE_FALSE(wide.has_value());
    CHECK(wide.error().find("square") != std::string::npos);

    const auto slash = ftk::create_desktop_entry({.id = "a/b", .program = "/a", .icon = folder.icon}, folder.data);
    REQUIRE_FALSE(slash.has_value());

    const auto nowhere = ftk::create_desktop_entry({.id = "a", .program = "/a", .icon = folder.icon}, {});
    REQUIRE_FALSE(nowhere.has_value());
    CHECK_FALSE(fs::exists(folder.data));
}
