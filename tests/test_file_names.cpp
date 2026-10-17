#include "ftk/platform/file_names.hpp"
#include "ftk/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using ftk::platform::free_name;
using ftk::platform::free_names;
using ftk::platform::NameParts;

namespace {

// An empty folder of its own for each test.
fs::path fresh_folder(const char* name) {
    const fs::path folder =
        fs::temp_directory_path() / ("ftk_file_names_" + std::to_string(ftk::platform::process_id())) / name;
    fs::remove_all(folder);
    fs::create_directories(folder);
    return folder;
}

void touch(const fs::path& file) { std::ofstream(file) << "x"; }

} // namespace

TEST_CASE("A name nothing has is kept; a taken one is numbered before its extension", "[file_names]") {
    const fs::path folder = fresh_folder("one");
    CHECK(free_name(folder, {.stem = "chair", .extension = ".gltf"}) == "chair.gltf");
    touch(folder / "chair.gltf");
    CHECK(free_name(folder, {.stem = "chair", .extension = ".gltf"}) == "chair_1.gltf");
    touch(folder / "chair_1.gltf");
    CHECK(free_name(folder, {.stem = "chair", .extension = ".gltf"}) == "chair_2.gltf");
    // Taken by a folder as much as by a file.
    fs::create_directory(folder / "new_folder");
    CHECK(free_name(folder, {.stem = "new_folder"}) == "new_folder_1");
    // A name already numbered is numbered again.
    CHECK(free_name(folder, {.stem = "chair_1", .extension = ".gltf"}) == "chair_1_1.gltf");
}

TEST_CASE("Names that go together are numbered alike, free for all of them", "[file_names]") {
    const fs::path folder = fresh_folder("set");
    const std::vector<NameParts> model{{.stem = "chair", .extension = ".gltf"}, {.stem = "chair", .extension = ".bin"}};
    CHECK(free_names(folder, model) == std::vector<std::string>{"chair.gltf", "chair.bin"});

    // Only the buffer is taken: both are numbered, so the model keeps naming its own.
    touch(folder / "chair.bin");
    CHECK(free_names(folder, model) == std::vector<std::string>{"chair_1.gltf", "chair_1.bin"});
    // The first number one of them is free at but not the other is passed.
    touch(folder / "chair_1.gltf");
    touch(folder / "chair_2.bin");
    CHECK(free_names(folder, model) == std::vector<std::string>{"chair_3.gltf", "chair_3.bin"});

    // A texture going in a folder of its own is looked for there, not beside
    // the model.
    fs::create_directory(folder / "textures");
    touch(folder / "textures" / "chair.png");
    touch(folder / "chair_2.png");
    const std::vector<NameParts> textured{{.stem = "chair", .extension = ".gltf"},
                                          {.stem = "chair", .extension = ".png", .within = "textures"}};
    CHECK(free_names(folder, textured) == std::vector<std::string>{"chair_2.gltf", "chair_2.png"});
}
