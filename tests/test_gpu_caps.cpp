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

TEST_CASE("A GPU without block compression is named short of it", "[gpu][caps]") {
    CHECK(missing_caps(Caps{.block_compression = true}, Caps{.block_compression = true}).empty());
    CHECK(missing_caps(Caps{}, Caps{.block_compression = true}) ==
          std::vector<std::string>{"block-compressed textures (BC4 to BC7)"});
}

TEST_CASE("A GPU that cannot leave a colour target unbound is named short of it", "[gpu][caps]") {
    CHECK(missing_caps(Caps{.unbound_targets = true}, Caps{.unbound_targets = true}).empty());
    CHECK(missing_caps(Caps{}, Caps{.unbound_targets = true}) ==
          std::vector<std::string>{"colour targets left unbound while pipelines draw to them"});
}

TEST_CASE("A GPU without fragment shader barycentrics is named short of them", "[gpu][caps]") {
    CHECK(missing_caps(Caps{.fragment_barycentrics = true}, Caps{.fragment_barycentrics = true}).empty());
    CHECK(missing_caps(Caps{}, Caps{.fragment_barycentrics = true}) ==
          std::vector<std::string>{"fragment shader barycentrics"});
}

TEST_CASE("A limit is met by any GPU that reaches it", "[gpu][caps]") {
    const Caps gpu{.mesh_shaders = true, .mesh_max_output_vertices = 256, .mesh_max_output_primitives = 256};
    CHECK(missing_caps(gpu, Caps{.mesh_shaders = true, .mesh_max_output_vertices = 256}).empty());
    CHECK(missing_caps(gpu, Caps{.mesh_max_output_vertices = 257}) ==
          std::vector<std::string>{"mesh shaders that output 257 vertices"});
}

TEST_CASE("A GPU whose stages hand on too few floats is named short of them", "[gpu][caps]") {
    const Caps required{.mesh_shaders = true,
                        .vertex_max_output_components = 68,
                        .mesh_max_output_components = 68,
                        .fragment_max_input_components = 68};
    const Caps desktop{.mesh_shaders = true,
                       .vertex_max_output_components = 128,
                       .mesh_max_output_components = 128,
                       .fragment_max_input_components = 128};
    CHECK(missing_caps(desktop, required).empty());
    CHECK(missing_caps(Caps{.mesh_shaders = true}, required) ==
          std::vector<std::string>{"vertex shaders that output 68 floats", "mesh shaders that output 68 floats",
                                   "fragment shaders that read 68 floats"});
}
