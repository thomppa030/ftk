#include "import/source_inspection.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("fjimport's summary line is read", "[import]") {
    const auto summary = fjell::parse_source_summary(
        R"({"animations":127,"bones":65,"embedded_textures":5,"materials":3,"meshes":2,"texture_files":1,)"
        R"("triangles":13744,"vertices":8546})");
    REQUIRE(summary.has_value());
    CHECK(summary->meshes == 2);
    CHECK(summary->triangles == 13744);
    CHECK(summary->embedded_textures == 5);
    CHECK(summary->texture_files == 1);
    CHECK(summary->bones == 65);
    CHECK(summary->animations == 127);
}

TEST_CASE("Lines that aren't a summary are passed over", "[import]") {
    CHECK_FALSE(fjell::parse_source_summary("importing crate.glb ...").has_value());
    CHECK_FALSE(fjell::parse_source_summary(R"({"phase":"loading","percent":0})").has_value());
    CHECK_FALSE(fjell::parse_source_summary("").has_value());
}
