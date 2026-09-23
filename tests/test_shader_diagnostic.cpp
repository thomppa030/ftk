#include "renderer/shader_diagnostic.hpp"

#include <catch2/catch_test_macros.hpp>

using fjell::first_shader_error;

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
