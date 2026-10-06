#include "ftk/shader/shader_diagnostic.hpp"

#include <catch2/catch_test_macros.hpp>

using ftk::first_shader_error;
using ftk::shader_errors;

TEST_CASE("A compile error names its file, line and message", "[shader]") {
    const auto d = first_shader_error(
        "shaders/sea.fjsl:42: error: 'foam_bias' : undeclared identifier\n"
        "shaders/sea.fjsl:43: error: 'other' : undeclared identifier\n"
        "2 errors generated.\n");
    CHECK(d.file == "shaders/sea.fjsl");
    CHECK(d.line == 42);
    CHECK(d.message == "'foam_bias' : undeclared identifier");
    CHECK(d.count == 2);
}

TEST_CASE("A file with a drive letter keeps it", "[shader]") {
    const auto d = first_shader_error("C:/game/shaders/sea.fjsl:7: error: syntax error\n");
    CHECK(d.file == "C:/game/shaders/sea.fjsl");
    CHECK(d.line == 7);
}

TEST_CASE("Text that isn't a compiler error is its own first line", "[shader]") {
    const auto d = first_shader_error("\nunknown hint 'hint_colour' on uniform tint\nmore\n");
    CHECK(d.file.empty());
    CHECK(d.line == 0);
    CHECK(d.message == "unknown hint 'hint_colour' on uniform tint");
    CHECK(d.count == 1);
}

TEST_CASE("No text is no error", "[shader]") {
    CHECK(first_shader_error("").count == 0);
}

TEST_CASE("An error with a column names it as well", "[shader]") {
    const auto d = first_shader_error("shaders/sea.fjsl:12:5: error: unknown hint 'hint_colour'\n");
    CHECK(d.file == "shaders/sea.fjsl");
    CHECK(d.line == 12);
    CHECK(d.column == 5);
    CHECK(d.message == "unknown hint 'hint_colour'");

    const auto drive = first_shader_error("C:/game/shaders/sea.fjsl:12:5: error: unknown hint\n");
    CHECK(drive.file == "C:/game/shaders/sea.fjsl");
    CHECK(drive.line == 12);
    CHECK(drive.column == 5);

    // glslc's own name no column.
    CHECK(first_shader_error("C:/game/shaders/sea.fjsl:7: error: syntax error\n").column == 0);
}

TEST_CASE("Every error of a compile is read, in its order", "[shader]") {
    const auto errors = shader_errors(
        "shaders/sea.fjsl:42: error: 'foam_bias' : undeclared identifier\n"
        "shaders/sea.fjsl:58: warning: unused variable\n"
        "/engine/shaders/include/waves.glsl:9: error: 'albedo' : wrong operand types\n"
        "2 errors generated.\n");
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].file == "shaders/sea.fjsl");
    CHECK(errors[0].line == 42);
    CHECK(errors[0].message == "'foam_bias' : undeclared identifier");
    CHECK(errors[1].file == "/engine/shaders/include/waves.glsl");
    CHECK(errors[1].line == 9);
    CHECK(errors[1].message == "'albedo' : wrong operand types");

    const auto other = shader_errors("\nFJSL file not found: shaders/gone.fjsl\n");
    REQUIRE(other.size() == 1);
    CHECK(other[0].file.empty());
    CHECK(other[0].message == "FJSL file not found: shaders/gone.fjsl");
    CHECK(shader_errors("").empty());
}
