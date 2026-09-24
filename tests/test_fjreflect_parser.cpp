#include "fjreflect/parser.hpp"
#include "platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Parses a header written to a temporary file, the way fjreflect reads a
// project's scripts.
fjreflect::ParseResult parse_header(const std::string& name, const std::string& text) {
    auto path = fs::temp_directory_path() /
                ("fjreflect_test_" + std::to_string(fjell::platform::process_id()) + "_" + name);
    std::ofstream(path) << text;
    auto result = fjreflect::parse_file(path.string());
    fs::remove(path);
    return result;
}

const fjreflect::FieldInfo* find_field(const fjreflect::ParseResult& result, const std::string& name) {
    for (const auto& component : result.components) {
        for (const auto& field : component.fields) {
            if (field.name == name) return &field;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("A trailing comment is not part of the field", "[fjreflect]") {
    auto result = parse_header("trailing.hpp", R"(
class hud_controller : public fjell::UiWidget {
  FJELL_UI_WIDGET(hud_controller)

public:
  [[fj::bind]] std::string selected;       // hotkey of the selected slot, "1".."8"
  [[fj::bind]] std::string label;          /* shown above the hotbar */
  [[fj::bind]] int hp = 0;                 // with an initializer
};
)");
    REQUIRE(result.errors.empty());
    REQUIRE(result.components.size() == 1);
    REQUIRE(result.components[0].fields.size() == 3);

    const auto* selected = find_field(result, "selected");
    REQUIRE(selected != nullptr);
    REQUIRE(selected->cpp_type == "std::string");
    REQUIRE(selected->is_bind);
    REQUIRE(find_field(result, "label") != nullptr);
    REQUIRE(find_field(result, "hp")->cpp_type == "int");
}

TEST_CASE("A commented-out attribute declares nothing", "[fjreflect]") {
    auto result = parse_header("commented.hpp", R"(
class Mover : public fjell::ScriptComponent {
  FJELL_COMPONENT(Mover)

public:
  // [[fj::property]] float old_speed = 1.0f;
  /* [[fj::property]] float older_speed = 1.0f; */
  /// Tag a field with [[fj::property]] to show it in the inspector.
  [[fj::property]] float speed = 2.0f;
};
)");
    REQUIRE(result.errors.empty());
    REQUIRE(result.components.size() == 1);
    REQUIRE(result.components[0].fields.size() == 1);
    REQUIRE(result.components[0].fields[0].name == "speed");
}

TEST_CASE("A brace in a comment leaves the class intact", "[fjreflect]") {
    auto result = parse_header("brace.hpp", R"(
class Mover : public fjell::ScriptComponent {
  FJELL_COMPONENT(Mover)

public:
  // an unmatched { in a comment
  [[fj::property]] float speed = 2.0f;
  /* and one } the other way */
  [[fj::property]] int lives = 3;
};
)");
    REQUIRE(result.errors.empty());
    REQUIRE(find_field(result, "speed") != nullptr);
    REQUIRE(find_field(result, "lives") != nullptr);
}

TEST_CASE("Comment markers inside literals are text", "[fjreflect]") {
    auto result = parse_header("literals.hpp", R"HDR(
class Link : public fjell::ScriptComponent {
  FJELL_COMPONENT(Link)

public:
  [[fj::property]] std::string url = "http://fjell.dev/*not a comment*/"; // but this is
  [[fj::property]] int budget = 1'000'000; // a digit separator, then a comment
  [[fj::property]] std::string raw = R"(// still text)"; // a comment
};
)HDR");
    REQUIRE(result.errors.empty());
    REQUIRE(result.components.size() == 1);
    REQUIRE(result.components[0].fields.size() == 3);
    REQUIRE(find_field(result, "url") != nullptr);
    REQUIRE(find_field(result, "budget")->cpp_type == "int");
    REQUIRE(find_field(result, "raw") != nullptr);
}

TEST_CASE("Line numbers still point into the file after a block comment", "[fjreflect]") {
    auto result = parse_header("lines.hpp",
        "class Mover : public fjell::ScriptComponent {\n"   // 1
        "  FJELL_COMPONENT(Mover)\n"                        // 2
        "  /* a block comment\n"                            // 3
        "     over three\n"                                 // 4
        "     lines */\n"                                   // 5
        "public:\n"                                         // 6
        "  [[fj::property]] float speed = 2.0f;\n"          // 7
        "};\n");
    REQUIRE(result.errors.empty());
    REQUIRE(find_field(result, "speed") != nullptr);
    REQUIRE(find_field(result, "speed")->line == 7);
}
