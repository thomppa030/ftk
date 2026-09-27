#include "ui/folder_colors.hpp"
#include "platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace fjell;
namespace fs = std::filesystem;

namespace {

struct ProjectDir {
    fs::path root;
    ProjectDir() {
        root = fs::temp_directory_path() /
               ("fjell_folder_colors_test_" + std::to_string(platform::process_id()));
        fs::remove_all(root);
        fs::create_directories(root / "Art" / "Characters");
        fs::create_directories(root / "Artwork");
        fs::create_directories(root / "Audio");
    }
    ~ProjectDir() { fs::remove_all(root); }
    [[nodiscard]] fs::path file() const { return root / ".fjell" / "folder_colors.json"; }
};

const glm::vec3 RED{1.0f, 0.0f, 0.0f};
const glm::vec3 BLUE{0.0f, 0.0f, 1.0f};

} // namespace

TEST_CASE("Folder colours survive a save and a load", "[folder_colors]") {
    ProjectDir project;
    FolderColors colors;
    colors.load(project.root);
    colors.set(project.root / "Art", RED);
    colors.set(project.root / "Art" / "Characters", BLUE);
    REQUIRE(colors.save());
    REQUIRE(fs::exists(project.file()));

    FolderColors reloaded;
    reloaded.load(project.root);
    CHECK(reloaded.find(project.root / "Art") == RED);
    CHECK(reloaded.find(project.root / "Art" / "Characters") == BLUE);
    CHECK_FALSE(reloaded.find(project.root / "Audio"));
}

TEST_CASE("A folder's colour is not inherited by its subfolders", "[folder_colors]") {
    ProjectDir project;
    FolderColors colors;
    colors.load(project.root);
    colors.set(project.root / "Art", RED);
    CHECK_FALSE(colors.find(project.root / "Art" / "Characters"));
}

TEST_CASE("The project root and outside folders take no colour", "[folder_colors]") {
    ProjectDir project;
    FolderColors colors;
    colors.load(project.root);
    colors.set(project.root, RED);
    colors.set(project.root.parent_path(), RED);
    CHECK_FALSE(colors.find(project.root));
    CHECK_FALSE(colors.find(project.root.parent_path()));
    REQUIRE(colors.save());
    CHECK_FALSE(fs::exists(project.file()));
}

TEST_CASE("Moving a folder carries its subfolders' colours", "[folder_colors]") {
    ProjectDir project;
    FolderColors colors;
    colors.load(project.root);
    colors.set(project.root / "Art", RED);
    colors.set(project.root / "Art" / "Characters", BLUE);
    // A sibling whose name starts the same is not beneath it.
    colors.set(project.root / "Artwork", BLUE);

    colors.moved(project.root / "Art", project.root / "Audio" / "Art");
    CHECK_FALSE(colors.find(project.root / "Art"));
    CHECK_FALSE(colors.find(project.root / "Art" / "Characters"));
    CHECK(colors.find(project.root / "Audio" / "Art") == RED);
    CHECK(colors.find(project.root / "Audio" / "Art" / "Characters") == BLUE);
    CHECK(colors.find(project.root / "Artwork") == BLUE);
}

TEST_CASE("Deleting a folder drops its subfolders' colours", "[folder_colors]") {
    ProjectDir project;
    FolderColors colors;
    colors.load(project.root);
    colors.set(project.root / "Art", RED);
    colors.set(project.root / "Art" / "Characters", BLUE);
    colors.set(project.root / "Artwork", BLUE);

    colors.removed(project.root / "Art");
    CHECK_FALSE(colors.find(project.root / "Art"));
    CHECK_FALSE(colors.find(project.root / "Art" / "Characters"));
    CHECK(colors.find(project.root / "Artwork") == BLUE);
}

TEST_CASE("Colours of folders gone from disk are dropped on load", "[folder_colors]") {
    ProjectDir project;
    {
        FolderColors colors;
        colors.load(project.root);
        colors.set(project.root / "Audio", RED);
        colors.set(project.root / "Artwork", BLUE);
        REQUIRE(colors.save());
    }
    fs::remove_all(project.root / "Audio");

    FolderColors colors;
    colors.load(project.root);
    CHECK_FALSE(colors.find(project.root / "Audio"));
    CHECK(colors.find(project.root / "Artwork") == BLUE);
    REQUIRE(colors.save());

    std::ifstream in(project.file());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("Audio") == std::string::npos);
}

TEST_CASE("An unreadable file leaves no colours", "[folder_colors]") {
    ProjectDir project;
    fs::create_directories(project.file().parent_path());
    std::ofstream(project.file()) << "{ not json";

    FolderColors colors;
    colors.load(project.root);
    CHECK_FALSE(colors.find(project.root / "Art"));
}

TEST_CASE("Folder colours round-trip through hex", "[folder_colors]") {
    CHECK(FolderColors::to_hex({0.831f, 0.627f, 0.329f}) == "#d4a054");
    CHECK(FolderColors::to_hex({2.0f, -1.0f, 0.5f}) == "#ff0080");

    auto parsed = FolderColors::from_hex("#D4A054");
    REQUIRE(parsed);
    CHECK(FolderColors::to_hex(*parsed) == "#d4a054");

    CHECK_FALSE(FolderColors::from_hex("d4a054"));
    CHECK_FALSE(FolderColors::from_hex("#d4a05"));
    CHECK_FALSE(FolderColors::from_hex("#-1a054"));
    CHECK_FALSE(FolderColors::from_hex("#zza054"));
}
