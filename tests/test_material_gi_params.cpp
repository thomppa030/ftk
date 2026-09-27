#include "renderer/resources/material_gi_params.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <map>

using namespace fjell;

namespace {

// Reads like the asset loader: the material's value when it has one, the
// shader default otherwise.
UniformValueReader values(std::map<std::string, std::vector<float>> overrides) {
    return [overrides = std::move(overrides)](const FjslUniform& u) {
        auto it = overrides.find(u.name);
        return it != overrides.end() ? it->second : u.default_value;
    };
}

void require_vec3(glm::vec3 v, float x, float y, float z) {
    REQUIRE(v.x == Catch::Approx(x));
    REQUIRE(v.y == Catch::Approx(y));
    REQUIRE(v.z == Catch::Approx(z));
}

} // namespace

TEST_CASE("Tagged colour and strength multiply into emission", "[material_gi]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 glow : hint_color, hint_emission = vec3(1.0, 0.5, 0.25);
        uniform float strength : hint_range(0.0, 20.0, 0.1), hint_emission = 4.0;
        void fragment() { EMISSION += glow * strength; }
    )");
    auto gi = derive_material_gi_params(meta, values({}), false);
    require_vec3(gi.emission, 4.0f, 2.0f, 1.0f);
}

TEST_CASE("Material values override shader defaults in the derivation", "[material_gi]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 glow : hint_color, hint_emission = vec3(1.0);
        uniform float strength : hint_emission = 1.0;
        void fragment() {}
    )");
    auto gi = derive_material_gi_params(meta, values({
        {"glow", {0.0f, 1.0f, 0.0f}},
        {"strength", {3.0f}},
    }), false);
    require_vec3(gi.emission, 0.0f, 3.0f, 0.0f);
}

TEST_CASE("Untagged shader without an emissive texture emits nothing", "[material_gi]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 tint : hint_color = vec3(1.0);
        void fragment() {}
    )");
    auto gi = derive_material_gi_params(meta, values({}), false);
    require_vec3(gi.emission, 0.0f, 0.0f, 0.0f);
}

TEST_CASE("Untagged shader with an emissive texture shows it at full strength", "[material_gi]") {
    // A texture-only emitter has no uniform to derive from; the factor must
    // default to one so the texture is neither hidden nor scaled.
    auto meta = parse_fjsl("shader_type spatial;\nvoid fragment() {}");
    auto gi = derive_material_gi_params(meta, values({}), true);
    require_vec3(gi.emission, 1.0f, 1.0f, 1.0f);
}

TEST_CASE("A tagged factor applies to a bound texture rather than defaulting", "[material_gi]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float strength : hint_emission = 2.5;
        void fragment() {}
    )");
    auto gi = derive_material_gi_params(meta, values({}), true);
    require_vec3(gi.emission, 2.5f, 2.5f, 2.5f);
}

TEST_CASE("Uniform names alone do not make a shader emit", "[material_gi]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 emission_color : hint_color = vec3(1.0, 0.7, 0.2);
        uniform float emission_strength : hint_range(0.0, 20.0, 0.1) = 5.0;
        void fragment() {}
    )");
    auto gi = derive_material_gi_params(meta, values({}), false);
    require_vec3(gi.emission, 0.0f, 0.0f, 0.0f);
}

TEST_CASE("Untagged uniforms next to a tagged one are ignored", "[material_gi]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 emission_color : hint_color = vec3(9.0);
        uniform vec3 real : hint_emission = vec3(1.0, 2.0, 3.0);
        void fragment() {}
    )");
    auto gi = derive_material_gi_params(meta, values({}), false);
    require_vec3(gi.emission, 1.0f, 2.0f, 3.0f);
}

TEST_CASE("A uniform the reader cannot supply is skipped", "[material_gi]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 glow : hint_emission = vec3(2.0);
        uniform float strength : hint_emission = 3.0;
        void fragment() {}
    )");
    auto gi = derive_material_gi_params(meta, [](const FjslUniform& u) {
        return u.name == "strength" ? std::vector<float>{} : u.default_value;
    }, false);
    require_vec3(gi.emission, 2.0f, 2.0f, 2.0f);
}

TEST_CASE("Tagged albedo uniforms multiply into the albedo factor", "[material_gi]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec4 colour : hint_color, hint_albedo = vec4(0.2, 0.4, 0.6, 1.0);
        uniform float brightness : hint_range(0.0, 2.0, 0.1), hint_albedo = 0.5;
        void fragment() {}
    )");
    auto gi = derive_material_gi_params(meta, values({}), false);
    require_vec3(gi.albedo, 0.1f, 0.2f, 0.3f);

    auto overridden = derive_material_gi_params(meta, values({{"brightness", {2.0f}}}), false);
    require_vec3(overridden.albedo, 0.4f, 0.8f, 1.2f);
}

TEST_CASE("A colour without hint_albedo does not become the albedo", "[material_gi]") {
    // A tint that only mixes into the texture cannot be expressed as a
    // factor, so the shader leaves it untagged and the texture stands.
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 tint : hint_color = vec3(0.2, 0.4, 0.6);
        uniform float strength : hint_range(0.0, 1.0, 0.01) = 0.5;
        void fragment() { ALBEDO = mix(ALBEDO, ALBEDO * tint, strength); }
    )");
    require_vec3(derive_material_gi_params(meta, values({}), false).albedo, 1.0f, 1.0f, 1.0f);

    auto none = parse_fjsl("shader_type spatial;\nvoid fragment() {}");
    require_vec3(derive_material_gi_params(none, values({}), false).albedo, 1.0f, 1.0f, 1.0f);
}
