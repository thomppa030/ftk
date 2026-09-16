#include "renderer/resources/fjinst_io.hpp"
#include "platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace fjell;
namespace fs = std::filesystem;

namespace {

fs::path temp_inst(const char* name) {
    auto dir = fs::temp_directory_path() /
               ("fjell_fjinst_test_" + std::to_string(platform::process_id()));
    fs::create_directories(dir);
    return dir / name;
}

} // namespace

TEST_CASE("Instance set round-trips every field", "[fjinst]") {
    auto path = temp_inst("trees.fjinst");
    FjInstData data;
    data.records.push_back({{1.0F, 2.0F, 3.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F}, 7, 0.25F});
    data.records.push_back({{-4.0F, 0.5F, 9.0F}, {0.0F, 0.7071F, 0.0F, 0.7071F}, {2.0F, 2.0F, 2.0F}, 0, 0.0F});
    REQUIRE(save_fjinst(path.string(), data));

    auto loaded = load_fjinst(path.string());
    REQUIRE(loaded);
    REQUIRE(loaded->header.version == FJINST_VERSION);
    REQUIRE(loaded->header.count == 2);
    REQUIRE(loaded->records.size() == 2);
    REQUIRE(loaded->records[0].position[2] == Catch::Approx(3.0F));
    REQUIRE(loaded->records[0].anchor_node == 7);
    REQUIRE(loaded->records[0].surface_offset == Catch::Approx(0.25F));
    REQUIRE(loaded->records[1].rotation[1] == Catch::Approx(0.7071F));
    REQUIRE(loaded->records[1].scale[0] == Catch::Approx(2.0F));
}

TEST_CASE("An empty instance set is a valid file", "[fjinst]") {
    auto path = temp_inst("empty.fjinst");
    REQUIRE(save_fjinst(path.string(), FjInstData{}));
    auto loaded = load_fjinst(path.string());
    REQUIRE(loaded);
    REQUIRE(loaded->records.empty());
}

TEST_CASE("Bad magic, wrong version and truncation are refused", "[fjinst]") {
    auto path = temp_inst("bad.fjinst");
    {
        FjInstHeader header{};
        header.magic = 0x12345678;
        header.version = FJINST_VERSION;
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(&header), sizeof(header));
    }
    REQUIRE_FALSE(load_fjinst(path.string()));
    {
        FjInstHeader header{};
        header.magic = FJINST_MAGIC;
        header.version = FJINST_VERSION + 1;
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(&header), sizeof(header));
    }
    REQUIRE_FALSE(load_fjinst(path.string()));
    {
        FjInstHeader header{};
        header.magic = FJINST_MAGIC;
        header.version = FJINST_VERSION;
        header.count = 3;
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(&header), sizeof(header));
    }
    REQUIRE_FALSE(load_fjinst(path.string()));
    REQUIRE_FALSE(load_fjinst(temp_inst("missing.fjinst").string()));
}
