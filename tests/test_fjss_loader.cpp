#include "ui/game_ui/fjss_loader.hpp"
#include "ui/game_ui/fjui_serializer.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace fjell;
namespace fs = std::filesystem;

static struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

struct SheetDir {
    fs::path base;
    SheetDir() {
        base = fs::temp_directory_path() / ("fjell_fjss_loader_test_" + std::to_string(platform::process_id()));
        fs::create_directories(base / "shared");
    }
    ~SheetDir() { fs::remove_all(base); }
    void write(const char* rel, const char* text) { std::ofstream(base / rel) << text; }
    std::string path(const char* rel) const { return (base / rel).string(); }
};

static std::string selector_of(const StyleRule& rule) { return to_css(rule.selector); }

TEST_CASE("An import's rules are spliced where the import is written", "[import]") {
    SheetDir dir;
    dir.write("base.fjss", ".base { width: 1; }\n");
    dir.write("hud.fjss", ".before { width: 0; }\n@import \"base.fjss\";\n.after { width: 2; }\n");

    auto sheet = load_stylesheet(dir.path("hud.fjss"));
    REQUIRE(sheet.diagnostics.empty());
    REQUIRE(sheet.rules.size() == 3);
    REQUIRE(selector_of(sheet.rules[0]) == ".before");
    REQUIRE(selector_of(sheet.rules[1]) == ".base");
    REQUIRE(selector_of(sheet.rules[2]) == ".after");
    // Each rule keeps its own file
    REQUIRE(sheet.rules[1].source == dir.path("base.fjss"));
    REQUIRE(sheet.rules[2].source == dir.path("hud.fjss"));
    REQUIRE(sheet.files == std::vector<std::string>{dir.path("hud.fjss"), dir.path("base.fjss")});
}

TEST_CASE("Imports resolve relative to the importing file and nest", "[import]") {
    SheetDir dir;
    dir.write("shared/theme.fjss", ":root { --accent: #ff0000; }\n");
    dir.write("shared/buttons.fjss", "@import \"theme.fjss\";\n.button { color: var(--accent); }\n");
    dir.write("hud.fjss", "@import \"shared/buttons.fjss\";\n.hud { width: 1; }\n");

    auto sheet = load_stylesheet(dir.path("hud.fjss"));
    REQUIRE(sheet.diagnostics.empty());
    REQUIRE(sheet.rules.size() == 3);
    REQUIRE(selector_of(sheet.rules[0]) == ":root");
    REQUIRE(selector_of(sheet.rules[1]) == ".button");
    REQUIRE(selector_of(sheet.rules[2]) == ".hud");
    REQUIRE(sheet.files.size() == 3);
    // Both files' own import lines are kept for writing back
    REQUIRE(sheet.imports.size() == 2);
}

TEST_CASE("A missing import is reported at its line and the rest loads", "[import]") {
    SheetDir dir;
    dir.write("hud.fjss", ".a { width: 1; }\n@import \"nope.fjss\";\n.b { width: 2; }\n");
    auto sheet = load_stylesheet(dir.path("hud.fjss"));
    REQUIRE(sheet.rules.size() == 2);
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].severity == FjssSeverity::error);
    REQUIRE(sheet.diagnostics[0].line == 2);
    REQUIRE(sheet.diagnostics[0].source == dir.path("hud.fjss"));
    REQUIRE(sheet.diagnostics[0].message.find("nope.fjss") != std::string::npos);
}

TEST_CASE("A cycle is reported and does not hang", "[import]") {
    SheetDir dir;
    dir.write("a.fjss", "@import \"b.fjss\";\n.a { width: 1; }\n");
    dir.write("b.fjss", "@import \"a.fjss\";\n.b { width: 2; }\n");
    auto sheet = load_stylesheet(dir.path("a.fjss"));
    REQUIRE(sheet.rules.size() == 2);
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].message.find("imports itself") != std::string::npos);
}

TEST_CASE("Saving the importing file writes its @import lines and only its rules", "[import]") {
    SheetDir dir;
    dir.write("base.fjss", ".base { width: 1; }\n");
    dir.write("hud.fjss", "@import \"base.fjss\";\n.hud { width: 2; }\n");
    auto sheet = load_stylesheet(dir.path("hud.fjss"));

    auto hud = FjssSerializer::serialize(sheet, dir.path("hud.fjss"));
    REQUIRE(hud.find("@import \"base.fjss\";") != std::string::npos);
    REQUIRE(hud.find(".hud {") != std::string::npos);
    REQUIRE(hud.find(".base") == std::string::npos);

    auto base = FjssSerializer::serialize(sheet, dir.path("base.fjss"));
    REQUIRE(base.find("@import") == std::string::npos);
    REQUIRE(base.find(".base {") != std::string::npos);
}

TEST_CASE("A font face's src resolves against the file that declares it", "[font-face]") {
    SheetDir dir;
    dir.write("shared/Inter.fjfont", "{ \"source\": \"Inter.ttf\" }\n");
    dir.write("shared/theme.fjss",
              "@font-face { font-family: Inter; font-weight: 700; src: \"Inter.fjfont\"; }\n");
    dir.write("hud.fjss", "@import \"shared/theme.fjss\";\n"
                          "@font-face { font-family: Missing; src: \"nope.fjfont\"; }\n"
                          ".a { font-family: Inter; }\n");

    auto sheet = load_stylesheet(dir.path("hud.fjss"));
    REQUIRE(sheet.font_faces.size() == 2);
    const auto* inter = &sheet.font_faces[0];
    const auto* missing = &sheet.font_faces[1];
    if (inter->family != "Inter") std::swap(inter, missing);
    REQUIRE(inter->family == "Inter");
    REQUIRE(inter->weight == 700.0f);
    REQUIRE(fs::path(inter->resolved_src) == (dir.base / "shared" / "Inter.fjfont").lexically_normal());
    REQUIRE(missing->family == "Missing");
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].message.find("does not exist") != std::string::npos);
    REQUIRE(sheet.diagnostics[0].line == 2);
}
