#include "renderer/resources/fjsl_loader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace fjell;

namespace {

// .fjsl files in a directory of their own, which the locator finds by the
// path a material would write.
struct ShaderTree {
    std::filesystem::path root;

    ShaderTree() {
        root = std::filesystem::temp_directory_path()
             / ("fjsl_loader_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(root);
    }
    ~ShaderTree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    ShaderTree(const ShaderTree&) = delete;
    ShaderTree& operator=(const ShaderTree&) = delete;

    void write(const std::string& relative, const std::string& text) const {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    [[nodiscard]] FjslLoader loader() const {
        return FjslLoader([this](const std::string& reference) -> std::string {
            const auto file = root / reference;
            return std::filesystem::exists(file) ? file.generic_string() : std::string{};
        });
    }
};

const std::string SUN_FN =
    "shader_function;\n"
    "uniform float strength : hint_range(0.0, 1.0, 0.1) = 0.5;\n"
    "in vec3 dir;\n"
    "out vec3 color;\n"
    "void main() { color = dir * strength; }\n";

const std::string TEMPLATE = "/*FJSL_CUSTOM_TEXTURES*/\n/*FJSL_USER_CODE*/\n";

} // namespace

TEST_CASE("A shader is found through the locator and composed", "[fjsl][loader]") {
    ShaderTree tree;
    tree.write("lib/sun.fjsl", SUN_FN);
    tree.write("shaders/sky.fjsl",
               "shader_type sky;\n"
               "use sun = \"lib/sun.fjsl\"(dir: VIEW_DIR);\n"
               "void sky() {}\n");
    const FjslLoader loader = tree.loader();

    FjslProgram program = loader.load("shaders/sky.fjsl");
    CHECK(program.path == "shaders/sky.fjsl");
    REQUIRE(program.used_paths.size() == 1);
    CHECK(program.used_paths[0] == "lib/sun.fjsl");

    // The function's dial joins the program's under the name the GLSL gives
    // it, grouped under the wiring.
    const uint32_t own_params = program.metadata.custom_param_count;
    (void)generate_fragment(program, TEMPLATE);
    CHECK(program.metadata.custom_param_count == own_params + 1);
    bool found = false;
    for (const auto& u : program.metadata.uniforms) {
        if (u.name == "sun_strength") {
            found = true;
            CHECK(u.group == "sun");
        }
    }
    CHECK(found);
}

TEST_CASE("A program depends on its file and on the functions it composes", "[fjsl][loader]") {
    FjslProgram program;
    program.path = "shaders/water.fjsl";
    program.used_paths = {"lib/foam.fjsl"};

    CHECK(program.depends_on("/home/me/game/shaders/water.fjsl"));
    CHECK(program.depends_on("/opt/fjell/lib/foam.fjsl"));
    // A file whose name only ends the same is another file.
    CHECK_FALSE(program.depends_on("/home/me/game/shaders/deep_water.fjsl"));
    CHECK_FALSE(program.depends_on("/home/me/game/shaders/toon.fjsl"));
}

TEST_CASE("A shader that is nowhere, or that does not parse, says why", "[fjsl][loader]") {
    ShaderTree tree;
    tree.write("shaders/broken.fjsl", "shader_type sky;\nuse a = \"x.fjsl\"(V);\nvoid sky() {}\n");
    const FjslLoader loader = tree.loader();

    try {
        (void)loader.load("shaders/gone.fjsl");
        FAIL("a missing shader loaded");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()) == "FJSL file not found: shaders/gone.fjsl");
    }

    try {
        (void)loader.load("shaders/broken.fjsl");
        FAIL("a shader that does not parse loaded");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).starts_with("shaders/broken.fjsl:2:"));
    }
}
