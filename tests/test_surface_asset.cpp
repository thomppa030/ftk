#include "ftk/platform/platform.hpp"
#include "physics/surface_asset.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace fjell;
namespace fs = std::filesystem;

namespace {

fs::path temp_surface(const char* name) {
    auto dir = fs::temp_directory_path() /
               ("fjell_surface_test_" + std::to_string(ftk::platform::process_id()));
    fs::create_directories(dir);
    return dir / name;
}

} // namespace

TEST_CASE("Surface asset round-trips its name", "[surface]") {
    auto path = temp_surface("grass.fjsurface");
    SurfaceAsset asset;
    asset.name = "Grass";
    REQUIRE(save_surface_asset_file(path.string(), asset));

    auto loaded = load_surface_asset_file(path.string());
    REQUIRE(loaded);
    REQUIRE(loaded->name == "Grass");
}

TEST_CASE("Surface asset name defaults to the file stem", "[surface]") {
    auto path = temp_surface("rock.fjsurface");
    std::ofstream(path) << "{}\n";

    auto loaded = load_surface_asset_file(path.string());
    REQUIRE(loaded);
    REQUIRE(loaded->name == "rock");
}

TEST_CASE("A missing or malformed surface file is an error", "[surface]") {
    REQUIRE_FALSE(load_surface_asset_file(temp_surface("nope.fjsurface").string()));
    auto path = temp_surface("bad.fjsurface");
    std::ofstream(path) << "{ not json";
    REQUIRE_FALSE(load_surface_asset_file(path.string()));
}
