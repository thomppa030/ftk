#include "ftk/base/file_io.hpp"
#include "ftk/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::string_literals;

namespace {

using File = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

// A file of the test's own, empty, open for writing and reading back.
File fresh_file(const char* name) {
    const fs::path folder = fs::temp_directory_path() / ("ftk_file_io_" + std::to_string(ftk::platform::process_id()));
    fs::create_directories(folder);
    return {ftk::platform::open_file(folder / name, "w+b"), &std::fclose};
}

} // namespace

TEST_CASE("Values written whole read back whole", "[file_io]") {
    const File file = fresh_file("whole.bin");
    REQUIRE(file != nullptr);
    const std::vector<std::uint32_t> counts{7, 11, 13};
    const std::array<float, 2> sizes{0.5f, 2.0f};
    CHECK(ftk::write_all(file.get(), counts));
    CHECK(ftk::write_all(file.get(), sizes));
    CHECK(std::ftell(file.get()) == static_cast<long>(sizeof(counts[0]) * 3 + sizeof(sizes)));

    std::rewind(file.get());
    std::vector<std::uint32_t> counts_read(3);
    std::array<float, 2> sizes_read{};
    CHECK(ftk::read_all(file.get(), counts_read));
    CHECK(ftk::read_all(file.get(), std::span(sizes_read)));
    CHECK(counts_read == counts);
    CHECK(sizes_read == sizes);
}

TEST_CASE("No values are all of them: nothing is written or read, and both succeed", "[file_io]") {
    // An empty vector's data() may be null, which the C library must not be
    // handed even for no bytes; a section a file holds none of is common.
    const File file = fresh_file("none.bin");
    REQUIRE(file != nullptr);
    const std::vector<std::uint32_t> none;
    CHECK(ftk::write_all(file.get(), none));
    CHECK(std::ftell(file.get()) == 0);

    std::vector<std::uint32_t> none_read;
    CHECK(ftk::read_all(file.get(), none_read));
    CHECK(none_read.empty());
}

TEST_CASE("A file shorter than the values asked for reads as not all of them", "[file_io]") {
    const File file = fresh_file("short.bin");
    REQUIRE(file != nullptr);
    CHECK(ftk::write_all(file.get(), std::vector<std::uint16_t>{1, 2}));
    std::rewind(file.get());
    std::vector<std::uint16_t> three(3);
    CHECK_FALSE(ftk::read_all(file.get(), three));
}

TEST_CASE("A file reads whole as its bytes", "[file_io]") {
    const fs::path folder = fs::temp_directory_path() / ("ftk_file_io_" + std::to_string(ftk::platform::process_id()));
    fs::create_directories(folder);
    const fs::path path = folder / "lines.txt";
    const std::string written = "one line\r\nand a second, ending in a null\0"s;
    {
        const File file{ftk::platform::open_file(path, "wb"), &std::fclose};
        REQUIRE(file != nullptr);
        CHECK(ftk::write_all(file.get(), written));
    }
    const auto read = ftk::read_text(path);
    REQUIRE(read.has_value());
    CHECK(*read == written);

    const auto missing = ftk::read_text(folder / "missing.txt");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().find("missing.txt") != std::string::npos);
}
