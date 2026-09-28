#include "gpu/shader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fjell::gpu;

namespace {

// The test shaders in tests/shaders/, compiled by the build.
std::vector<uint32_t> spirv(const std::string& name) {
    const std::filesystem::path path = std::filesystem::path(FJELL_TEST_SHADER_DIR) / (name + ".spv");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    const auto bytes = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> words(bytes / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(bytes));
    return words;
}

ShaderLayout reflected(const std::string& name) {
    auto layout = reflect(spirv(name));
    REQUIRE(layout.has_value());
    return *layout;
}

} // namespace

TEST_CASE("reflection reads each kind of binding with its set, binding and name", "[gpu][shader]") {
    const ShaderLayout layout = reflected("reflect.comp");
    CHECK(layout.stages == ShaderStage::compute);

    const ShaderBinding* params = layout.find("params");
    REQUIRE(params != nullptr);
    CHECK(params->set == 0);
    CHECK(params->binding == 0);
    CHECK(params->kind == BindingKind::uniform_buffer);
    CHECK(params->block == "Params");

    CHECK(layout.find("source")->kind == BindingKind::sampled_texture);
    CHECK(layout.find("target")->kind == BindingKind::storage_texture);
    CHECK(layout.find("plain")->kind == BindingKind::texture);
    CHECK(layout.find("pointy")->kind == BindingKind::sampler);

    const ShaderBinding* counts = layout.find("counts_out");
    REQUIRE(counts != nullptr);
    CHECK(counts->kind == BindingKind::storage_buffer);
    CHECK(counts->set == 2);
    CHECK(counts->binding == 1);
}

TEST_CASE("a block without an instance name goes by its type's name", "[gpu][shader]") {
    const ShaderLayout layout = reflected("reflect.comp");
    const ShaderBinding* items = layout.find("Items");
    REQUIRE(items != nullptr);
    CHECK(items->block == "Items");
    CHECK(items->set == 2);
    CHECK(items->binding == 0);
}

TEST_CASE("arrays keep their length, and a runtime array reads as zero", "[gpu][shader]") {
    const ShaderLayout layout = reflected("reflect.comp");
    CHECK(layout.find("pair")->count == 2);
    CHECK(layout.find("textures")->count == 0);
    CHECK(layout.find("source")->count == 1);
}

TEST_CASE("bindings come ordered by set, then binding", "[gpu][shader]") {
    const ShaderLayout layout = reflected("reflect.comp");
    REQUIRE(layout.bindings.size() == 9);
    for (size_t i = 1; i < layout.bindings.size(); ++i) {
        const auto& before = layout.bindings[i - 1];
        const auto& after = layout.bindings[i];
        CHECK((before.set < after.set || (before.set == after.set && before.binding < after.binding)));
    }
}

TEST_CASE("reflection reads push data and the workgroup", "[gpu][shader]") {
    const ShaderLayout layout = reflected("reflect.comp");
    CHECK(layout.push_size == 16);
    CHECK(layout.push_stages == ShaderStage::compute);
    CHECK(layout.workgroup_size(ShaderStage::compute) == std::array<uint32_t, 3>{8, 4, 1});
}

TEST_CASE("a mesh stage's output limits are read", "[gpu][shader]") {
    const ShaderLayout layout = reflected("reflect.mesh");
    CHECK(layout.stages == ShaderStage::mesh);
    CHECK(layout.mesh_max_vertices == 64);
    CHECK(layout.mesh_max_primitives == 124);
    CHECK(layout.workgroup_size(ShaderStage::mesh) == std::array<uint32_t, 3>{32, 1, 1});
}

TEST_CASE("merging two stages joins their bindings and push data", "[gpu][shader]") {
    auto merged = merge(reflected("reflect.vert"), reflected("reflect.frag"));
    REQUIRE(merged.has_value());
    CHECK(merged->stages == (ShaderStage::vertex | ShaderStage::fragment));

    const ShaderBinding* camera = merged->find("camera");
    REQUIRE(camera != nullptr);
    CHECK(camera->stages == (ShaderStage::vertex | ShaderStage::fragment));

    const ShaderBinding* albedo = merged->find("albedo");
    REQUIRE(albedo != nullptr);
    CHECK(albedo->stages == ShaderStage::fragment);

    CHECK(merged->push_size == 80);
    CHECK(merged->push_stages == (ShaderStage::vertex | ShaderStage::fragment));
}

TEST_CASE("stages that disagree on a binding cannot be merged", "[gpu][shader]") {
    auto merged = merge(reflected("reflect.vert"), reflected("reflect_conflict.frag"));
    REQUIRE_FALSE(merged.has_value());
    CHECK(merged.error().find("set 0 binding 0") != std::string::npos);
}

TEST_CASE("what is not SPIR-V is refused with a reason", "[gpu][shader]") {
    const std::vector<uint32_t> nonsense{1, 2, 3, 4, 5, 6};
    auto layout = reflect(nonsense);
    REQUIRE_FALSE(layout.has_value());
    CHECK_FALSE(layout.error().empty());
}
