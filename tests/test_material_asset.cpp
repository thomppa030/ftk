#include "renderer/resources/material_asset.hpp"
#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdio>
#include <filesystem>

using namespace fjell;

namespace {

struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

std::string temp_fjmat() {
    auto path = std::filesystem::temp_directory_path() / "fjell_test_material.fjmat";
    return path.string();
}

} // namespace

TEST_CASE("A material without base keys reads the defaults", "[material_asset]") {
    MaterialAsset asset;
    asset.params["tint"] = {0.5f, 0.5f, 0.5f};
    auto base = read_base_params(asset);
    REQUIRE(base.metallic_factor == 1.0f);
    REQUIRE(base.roughness_factor == 1.0f);
    REQUIRE(base.uv_scale.x == 1.0f);
    REQUIRE(base.uv_scale.y == 1.0f);
}

TEST_CASE("Base factors round-trip through params and leave defaults out", "[material_asset]") {
    MaterialAsset asset;
    asset.params["tint"] = {0.5f, 0.5f, 0.5f};
    write_base_params(asset, {.metallic_factor = 0.25f, .roughness_factor = 0.6f,
                              .uv_scale = {1.0f, 1.0f}});

    REQUIRE(asset.params.contains("metallic_factor"));
    REQUIRE(asset.params.contains("roughness_factor"));
    REQUIRE_FALSE(asset.params.contains("uv_scale"));
    REQUIRE(asset.params.at("tint").size() == 3);

    auto base = read_base_params(asset);
    REQUIRE(base.metallic_factor == Catch::Approx(0.25f));
    REQUIRE(base.roughness_factor == Catch::Approx(0.6f));

    // Back at the default the key disappears again instead of lingering.
    write_base_params(asset, {});
    REQUIRE_FALSE(asset.params.contains("metallic_factor"));
    REQUIRE_FALSE(asset.params.contains("roughness_factor"));
}

TEST_CASE("Base factors survive a save and load", "[material_asset]") {
    MaterialAsset asset;
    asset.shader = "shaders/solid_lit.fjsl";
    asset.params["tint"] = {0.4f, 0.3f, 0.3f};
    write_base_params(asset, {.roughness_factor = 0.35f, .uv_scale = {2.0f, 3.0f}});

    auto path = temp_fjmat();
    save_material_asset(path, asset);
    auto loaded = load_material_asset(path);
    std::remove(path.c_str());

    auto base = read_base_params(loaded);
    REQUIRE(base.metallic_factor == 1.0f);
    REQUIRE(base.roughness_factor == Catch::Approx(0.35f));
    REQUIRE(base.uv_scale.x == Catch::Approx(2.0f));
    REQUIRE(base.uv_scale.y == Catch::Approx(3.0f));
    REQUIRE(loaded.params.at("tint")[0] == Catch::Approx(0.4f));
}
