#include "ftk/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path fresh_folder(const char* name) {
    auto folder = fs::temp_directory_path() / ("ftk_platform_files_test_" + std::to_string(ftk::platform::process_id())) /
                  name;
    fs::remove_all(folder);
    fs::create_directories(folder);
    return folder;
}

// A folder name with letters outside ASCII, spelled as its UTF-8 bytes so the
// test reads the same whatever code page a compiler reads the source in.
fs::path utf8_name(const char* text) {
    const std::string bytes(text);
    return std::u8string(bytes.begin(), bytes.end());
}

} // namespace

TEST_CASE("A file opens by its own name, letters outside ASCII included", "[platform][file]") {
    const fs::path folder = fresh_folder("open") / utf8_name("Sk\xc3\xb3gar");  // Skógar
    fs::create_directories(folder);
    const fs::path path = folder / "quick.fjsave";

    std::FILE* out = ftk::platform::open_file(path, "wb");
    REQUIRE(out != nullptr);
    REQUIRE(std::fputs("saved", out) >= 0);
    REQUIRE(std::fclose(out) == 0);

    std::FILE* in = ftk::platform::open_file(path, "rb");
    REQUIRE(in != nullptr);
    char read[8] = {};
    CHECK(std::fread(read, 1, 5, in) == 5);
    std::fclose(in);
    CHECK(std::string(read) == "saved");
}

TEST_CASE("A file that isn't there doesn't open for reading", "[platform][file]") {
    CHECK(ftk::platform::open_file(fresh_folder("missing") / "none.fjsave", "rb") == nullptr);
}
