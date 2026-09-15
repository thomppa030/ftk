#include "renderer/resources/fjterrain_io.hpp"
#include "platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <fstream>

using namespace fjell;
namespace fs = std::filesystem;

namespace {

fs::path temp_terrain(const char* name) {
    auto dir = fs::temp_directory_path() /
               ("fjell_fjterrain_test_" + std::to_string(platform::process_id()));
    fs::create_directories(dir);
    return dir / name;
}

// A 5x5 field of 2-quad patches with a ramp for heights.
FjTerrainData small_field() {
    FjTerrainData data;
    data.header.sample_count = 5;
    data.header.patch_quads = 2;
    data.header.world_size = 40.0F;
    data.header.height_scale = 10.0F;
    data.header.height_offset = -2.0F;
    data.header.height_format = FjTerrainHeightFormat::unorm16;
    data.heights.resize(25);
    for (size_t i = 0; i < data.heights.size(); ++i) {
        data.heights[i] = static_cast<float>(i) / 24.0F;
    }
    return data;
}

} // namespace

TEST_CASE("Terrain with no layers round-trips", "[fjterrain]") {
    auto path = temp_terrain("plain.fjterrain");
    auto data = small_field();
    REQUIRE(save_fjterrain(path.string(), data));

    auto loaded = load_fjterrain(path.string());
    REQUIRE(loaded);
    REQUIRE(loaded->header.version == FJTERRAIN_VERSION);
    REQUIRE(loaded->header.layer_count == 0);
    REQUIRE(loaded->header.weight_count == 0);
    REQUIRE(loaded->layers.empty());
    REQUIRE(loaded->names.empty());
    REQUIRE(loaded->weights.empty());
    REQUIRE(loaded->heights.size() == 25);
    REQUIRE(loaded->heights[24] == Catch::Approx(1.0F).margin(1.0F / 65535.0F));
}

TEST_CASE("Layer table and names survive without painted weights", "[fjterrain]") {
    auto path = temp_terrain("layers.fjterrain");
    auto data = small_field();
    data.names = {"materials/grass.fjmat", "", "materials/rock.fjmat", "surfaces/rock.fjsurface"};
    data.layers = {{0, 4.0F, 1}, {1, 4.0F, 1}, {2, 2.5F, 3}};
    REQUIRE(save_fjterrain(path.string(), data));

    auto loaded = load_fjterrain(path.string());
    REQUIRE(loaded);
    REQUIRE(loaded->header.layer_count == 3);
    REQUIRE(loaded->header.weight_count == 0);
    REQUIRE(loaded->weights.empty());
    REQUIRE(loaded->layers.size() == 3);
    REQUIRE(loaded->layers[2].uv_scale == Catch::Approx(2.5F));
    REQUIRE(loaded->names == data.names);
    REQUIRE(loaded->names[loaded->layers[1].material_path].empty());
    REQUIRE(loaded->names[loaded->layers[2].surface_path] == "surfaces/rock.fjsurface");
    REQUIRE(loaded->names[loaded->layers[0].surface_path].empty());
}

TEST_CASE("Painted weights round-trip byte for byte", "[fjterrain]") {
    auto path = temp_terrain("painted.fjterrain");
    auto data = small_field();
    data.names = {"a.fjmat", "b.fjmat", ""};
    data.layers = {{0, 4.0F, 2}, {1, 4.0F, 2}};
    data.header.weight_count = 3;
    data.weights.resize(3 * 3 * 4);
    for (size_t i = 0; i < data.weights.size(); ++i) {
        data.weights[i] = static_cast<uint8_t>((i * 37) & 0xFF);
    }
    REQUIRE(save_fjterrain(path.string(), data));

    auto loaded = load_fjterrain(path.string());
    REQUIRE(loaded);
    REQUIRE(loaded->header.weight_count == 3);
    REQUIRE(loaded->weights == data.weights);
    REQUIRE(loaded->names == data.names);
}

TEST_CASE("A layer naming a missing string is refused on save and load", "[fjterrain]") {
    auto path = temp_terrain("bad_layer.fjterrain");
    auto data = small_field();
    data.names = {"a.fjmat"};
    data.layers = {{0, 4.0F, 0}, {5, 4.0F, 0}};
    REQUIRE_FALSE(save_fjterrain(path.string(), data));
    data.layers = {{0, 4.0F, 7}};
    REQUIRE_FALSE(save_fjterrain(path.string(), data));
}

TEST_CASE("Weight array must match weight_count on save", "[fjterrain]") {
    auto path = temp_terrain("bad_weights.fjterrain");
    auto data = small_field();
    data.names = {"a.fjmat"};
    data.layers = {{0, 4.0F, 0}};
    data.header.weight_count = 4;
    data.weights.resize(7);
    REQUIRE_FALSE(save_fjterrain(path.string(), data));
}

TEST_CASE("Version 1 files load when they carry no layers", "[fjterrain]") {
    auto path = temp_terrain("v1.fjterrain");
    auto data = small_field();
    REQUIRE(save_fjterrain(path.string(), data));

    // Rewrite the version field in place: version 1 files were exactly this
    // layout without the layer or name tables.
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(f.is_open());
        const uint32_t version = 1;
        f.seekp(static_cast<std::streamoff>(offsetof(FjTerrainHeader, version)));
        f.write(reinterpret_cast<const char*>(&version), sizeof(version));
    }
    auto loaded = load_fjterrain(path.string());
    REQUIRE(loaded);
    REQUIRE(loaded->header.version == 1);
    REQUIRE(loaded->heights.size() == 25);
}

TEST_CASE("Version 2 layer records load with no surface", "[fjterrain]") {
    auto path = temp_terrain("v2.fjterrain");
    auto data = small_field();

    // Write the file the way version 2 did: header, heights, the two-field
    // layer records, then the name table.
    {
        FjTerrainHeader header = data.header;
        header.magic = FJTERRAIN_MAGIC;
        header.version = 2;
        header.layer_count = 2;
        header.weight_count = 0;
        std::ofstream f(path, std::ios::binary);
        REQUIRE(f.is_open());
        f.write(reinterpret_cast<const char*>(&header), sizeof(header));
        for (float h : data.heights) {
            const auto raw = static_cast<uint16_t>(h * 65535.0F);
            f.write(reinterpret_cast<const char*>(&raw), sizeof(raw));
        }
        const FjTerrainLayerV2 layers[2] = {{0, 4.0F}, {1, 2.0F}};
        f.write(reinterpret_cast<const char*>(layers), sizeof(layers));
        const uint32_t name_count = 2;
        f.write(reinterpret_cast<const char*>(&name_count), sizeof(name_count));
        for (const char* name : {"grass.fjmat", "rock.fjmat"}) {
            const auto length = static_cast<uint32_t>(std::string(name).size());
            f.write(reinterpret_cast<const char*>(&length), sizeof(length));
            f.write(name, length);
        }
    }

    auto loaded = load_fjterrain(path.string());
    REQUIRE(loaded);
    REQUIRE(loaded->layers.size() == 2);
    REQUIRE(loaded->names[loaded->layers[0].material_path] == "grass.fjmat");
    REQUIRE(loaded->names[loaded->layers[1].material_path] == "rock.fjmat");
    REQUIRE(loaded->layers[1].uv_scale == Catch::Approx(2.0F));
    REQUIRE(loaded->names[loaded->layers[0].surface_path].empty());
    REQUIRE(loaded->names[loaded->layers[1].surface_path].empty());

    // Saving writes the current version, and it reads back the same.
    REQUIRE(save_fjterrain(path.string(), *loaded));
    auto again = load_fjterrain(path.string());
    REQUIRE(again);
    REQUIRE(again->header.version == FJTERRAIN_VERSION);
    REQUIRE(again->names[again->layers[1].material_path] == "rock.fjmat");
}
