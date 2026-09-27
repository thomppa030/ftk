#include "renderer/resources/fjsl_compiler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

using namespace fjell;

namespace {

// A shader tree in a directory of its own: include/ for the compiler to hash,
// generated/ for what it writes, and .fjsl files the locator finds by the
// path a material would write.
struct ShaderTree {
    std::filesystem::path root;

    ShaderTree() {
        root = std::filesystem::temp_directory_path()
             / ("fjsl_compiler_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(root);
        write("shaders/include/a.glsl", "// a\n");
        write("shaders/include/tone/b.glsl", "// b\n");
    }
    ~ShaderTree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    ShaderTree(const ShaderTree&) = delete;
    ShaderTree& operator=(const ShaderTree&) = delete;

    // Written as bytes, so a line ends the same on every platform and the
    // pinned keys below hold everywhere.
    void write(const std::string& relative, const std::string& text) const {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    [[nodiscard]] std::string path(const std::string& relative) const { return (root / relative).generic_string(); }

    [[nodiscard]] FjslCompiler compiler() const {
        return FjslCompiler(path("shaders"), path("shaders/generated"),
                            [this](const std::string& reference) -> std::string {
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

TEST_CASE("A cache key is exactly what it was", "[fjsl][compiler]") {
    // Every SPIR-V the engine has cached is filed under this key. A change to
    // what goes into it recompiles every shader on every machine, so it has to
    // be a deliberate one: these values are FNV-1a over the include files,
    // the source, the defines, the stage and the generator's version, worked
    // out apart from the code.
    ShaderTree tree;
    tree.write("shaders/generated/x.frag", "void main() {}\n");
    const FjslCompiler compiler = tree.compiler();

    const FjslCompiler::Job shaped{tree.path("shaders/generated/x.frag"), "x", {"FJELL_UI_WORLD"}, "compute"};
    CHECK(compiler.cache_key(shaped) == 0x7e76e492e9072f89ULL);
    const FjslCompiler::Job plain{tree.path("shaders/generated/x.frag"), "x"};
    CHECK(compiler.cache_key(plain) == 0xbae69c53d4f95297ULL);
}

TEST_CASE("An edited include changes the key once it is hashed again", "[fjsl][compiler]") {
    ShaderTree tree;
    tree.write("shaders/generated/x.frag", "void main() {}\n");
    FjslCompiler compiler = tree.compiler();
    const FjslCompiler::Job job{tree.path("shaders/generated/x.frag"), "x"};
    const uint64_t before = compiler.cache_key(job);

    tree.write("shaders/include/tone/b.glsl", "// b, edited\n");
    CHECK(compiler.cache_key(job) == before);
    compiler.rehash_includes();
    CHECK(compiler.cache_key(job) != before);
}

TEST_CASE("A shader is found through the locator and composed", "[fjsl][compiler]") {
    ShaderTree tree;
    tree.write("lib/sun.fjsl", SUN_FN);
    tree.write("shaders/sky.fjsl",
               "shader_type sky;\n"
               "use sun = \"lib/sun.fjsl\"(dir: VIEW_DIR);\n"
               "void sky() {}\n");
    const FjslCompiler compiler = tree.compiler();

    FjslProgram program = compiler.load("shaders/sky.fjsl");
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

TEST_CASE("A program depends on its file and on the functions it composes", "[fjsl][compiler]") {
    FjslProgram program;
    program.path = "shaders/water.fjsl";
    program.used_paths = {"lib/foam.fjsl"};

    CHECK(program.depends_on("/home/me/game/shaders/water.fjsl"));
    CHECK(program.depends_on("/opt/fjell/lib/foam.fjsl"));
    // A file whose name only ends the same is another file.
    CHECK_FALSE(program.depends_on("/home/me/game/shaders/deep_water.fjsl"));
    CHECK_FALSE(program.depends_on("/home/me/game/shaders/toon.fjsl"));
}

TEST_CASE("A shader that is nowhere, or that does not parse, says why", "[fjsl][compiler]") {
    ShaderTree tree;
    tree.write("shaders/broken.fjsl", "shader_type sky;\nuse a = \"x.fjsl\"(V);\nvoid sky() {}\n");
    const FjslCompiler compiler = tree.compiler();

    try {
        (void)compiler.load("shaders/gone.fjsl");
        FAIL("a missing shader loaded");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()) == "FJSL file not found: shaders/gone.fjsl");
    }

    try {
        (void)compiler.load("shaders/broken.fjsl");
        FAIL("a shader that does not parse loaded");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).starts_with("shaders/broken.fjsl:2:"));
    }
}

TEST_CASE("Generated sources land in the generated directory", "[fjsl][compiler]") {
    ShaderTree tree;
    const FjslCompiler compiler = tree.compiler();
    const std::string path = compiler.write_generated("sky.frag", "void main() {}\n");
    CHECK(path == tree.path("shaders/generated/sky.frag"));
    CHECK(std::filesystem::exists(path));
    CHECK(compiler.read_include("tone/b.glsl") == "// b\n");
    CHECK(compiler.read_include("nothing.glsl").empty());
}
