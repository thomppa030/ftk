#include "renderer/resources/material_gi_params.hpp"
#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <map>

using namespace fjell;

namespace {

struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

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
    auto gi = derive_material_gi_params(meta, values({}));
    require_vec3(gi.emission, 4.0f, 2.0f, 1.0f);
    REQUIRE_FALSE(gi.emission_from_names);
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
    }));
    require_vec3(gi.emission, 0.0f, 3.0f, 0.0f);
}

TEST_CASE("Untagged shaders emit nothing", "[material_gi]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 tint : hint_color = vec3(1.0);
        void fragment() {}
    )");
    auto gi = derive_material_gi_params(meta, values({}));
    require_vec3(gi.emission, 0.0f, 0.0f, 0.0f);
    REQUIRE_FALSE(gi.emission_from_names);
}

TEST_CASE("Legacy emission_* names still emit, and are reported", "[material_gi]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 emission_color : hint_color = vec3(1.0, 0.7, 0.2);
        uniform float emission_strength : hint_range(0.0, 20.0, 0.1) = 5.0;
        void fragment() {}
    )");
    auto gi = derive_material_gi_params(meta, values({}));
    require_vec3(gi.emission, 5.0f, 3.5f, 1.0f);
    REQUIRE(gi.emission_from_names);
}

TEST_CASE("A hint_emission tag takes precedence over legacy names", "[material_gi]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 emission_color : hint_color = vec3(9.0);
        uniform vec3 real : hint_emission = vec3(1.0, 2.0, 3.0);
        void fragment() {}
    )");
    auto gi = derive_material_gi_params(meta, values({}));
    require_vec3(gi.emission, 1.0f, 2.0f, 3.0f);
    REQUIRE_FALSE(gi.emission_from_names);
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
    });
    require_vec3(gi.emission, 2.0f, 2.0f, 2.0f);
}

TEST_CASE("Albedo override needs exactly one hint_color vector", "[material_gi]") {
    auto one = parse_fjsl(R"(
        shader_type spatial;
        uniform vec4 colour : hint_color = vec4(0.2, 0.4, 0.6, 1.0);
        void fragment() { ALBEDO = colour.rgb; }
    )");
    auto gi = derive_material_gi_params(one, values({}));
    REQUIRE(gi.gi_albedo_override.has_value());
    require_vec3(*gi.gi_albedo_override, 0.2f, 0.4f, 0.6f);

    auto two = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 a : hint_color = vec3(1.0);
        uniform vec3 b : hint_color = vec3(0.0);
        void fragment() {}
    )");
    REQUIRE_FALSE(derive_material_gi_params(two, values({})).gi_albedo_override.has_value());

    auto none = parse_fjsl("shader_type spatial;\nvoid fragment() {}");
    REQUIRE_FALSE(derive_material_gi_params(none, values({})).gi_albedo_override.has_value());
}
