#include "ftk/gpu/device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace ftk::gpu;

TEST_CASE("A device that asks for nothing misses nothing", "[gpu][caps]") {
    const Caps bare{};
    CHECK(missing_caps(bare, Caps{}).empty());
    CHECK(missing_caps(Caps{.mesh_shaders = true, .ray_queries = true}, Caps{}).empty());
}

TEST_CASE("What a GPU lacks of what a program requires is named", "[gpu][caps]") {
    const Caps gpu{.max_samples = Samples::x4};
    const Caps required{.mesh_shaders = true, .max_samples = Samples::x8, .ray_queries = true};

    const std::vector<std::string> missing = missing_caps(gpu, required);
    REQUIRE(missing.size() == 3);
    CHECK(missing[0].starts_with("mesh shaders"));
    CHECK(missing[1] == "8x multisampling");
    CHECK(missing[2] == "ray queries");
}

TEST_CASE("A limit is met by any GPU that reaches it", "[gpu][caps]") {
    const Caps gpu{.mesh_shaders = true, .mesh_max_output_vertices = 256, .mesh_max_output_primitives = 256};
    CHECK(missing_caps(gpu, Caps{.mesh_shaders = true, .mesh_max_output_vertices = 256}).empty());
    CHECK(missing_caps(gpu, Caps{.mesh_max_output_vertices = 257}) ==
          std::vector<std::string>{"mesh shaders that output 257 vertices"});
}
