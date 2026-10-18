#include "ftk/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
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

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
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
    REQUIRE(ftk::platform::sync_file(out));
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

TEST_CASE("A replaced file takes the new one's contents", "[platform][file]") {
    const fs::path folder = fresh_folder("plain");
    write_text(folder / "slot", "old");
    write_text(folder / "slot.tmp", "new");

    REQUIRE(ftk::platform::replace_file(folder / "slot.tmp", folder / "slot"));
    CHECK(read_text(folder / "slot") == "new");
    CHECK_FALSE(fs::exists(folder / "slot.tmp"));
}

TEST_CASE("Replacing a file keeps the old one as its backup", "[platform][file]") {
    const fs::path folder = fresh_folder("backup");
    write_text(folder / "slot", "old");
    write_text(folder / "slot.tmp", "new");

    REQUIRE(ftk::platform::replace_file(folder / "slot.tmp", folder / "slot", folder / "slot.bak"));
    CHECK(read_text(folder / "slot") == "new");
    CHECK(read_text(folder / "slot.bak") == "old");
    CHECK_FALSE(fs::exists(folder / "slot.tmp"));
    CHECK_FALSE(fs::exists(folder / "slot.bak.new"));
}

TEST_CASE("The next replace makes the file it replaces the backup", "[platform][file]") {
    const fs::path folder = fresh_folder("rotate");
    write_text(folder / "slot", "second");
    write_text(folder / "slot.bak", "first");
    write_text(folder / "slot.tmp", "third");

    REQUIRE(ftk::platform::replace_file(folder / "slot.tmp", folder / "slot", folder / "slot.bak"));
    CHECK(read_text(folder / "slot") == "third");
    CHECK(read_text(folder / "slot.bak") == "second");
}

TEST_CASE("A first file has no backup to keep", "[platform][file]") {
    const fs::path folder = fresh_folder("first");
    write_text(folder / "slot.tmp", "new");

    REQUIRE(ftk::platform::replace_file(folder / "slot.tmp", folder / "slot", folder / "slot.bak"));
    CHECK(read_text(folder / "slot") == "new");
    CHECK_FALSE(fs::exists(folder / "slot.bak"));
}

TEST_CASE("A replace with nothing to put in place changes nothing", "[platform][file]") {
    const fs::path folder = fresh_folder("nothing");
    write_text(folder / "slot", "old");
    write_text(folder / "slot.bak", "older");

    CHECK_FALSE(ftk::platform::replace_file(folder / "slot.tmp", folder / "slot", folder / "slot.bak"));
    CHECK(read_text(folder / "slot") == "old");
    CHECK(read_text(folder / "slot.bak") == "older");
}
