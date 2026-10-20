#include "ftk/shader/shader_compiler.hpp"

#include "ftk/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ftk;

namespace {

// A shader tree in a directory of its own: shaders/include/ for the compiler
// to hash and shaders/generated/ for what it writes, and room beside them for
// a game's own include directory.
struct ShaderTree {
    std::filesystem::path root;

    ShaderTree() {
        root = std::filesystem::temp_directory_path()
             / ("shader_compiler_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

    // Version 1 is what the engine's FJSL passes, so the keys pinned below
    // are the ones its SPIR-V cache holds. With no directories given, the
    // compiler looks in shaders/include alone.
    [[nodiscard]] ShaderCompiler compiler(std::vector<std::string> include_dirs = {}) const {
        if (include_dirs.empty()) include_dirs = {path("shaders/include")};
        return ShaderCompiler({.include_dirs = std::move(include_dirs),
                               .generated_dir = path("shaders/generated"),
                               .cache_dir = path("shaders/generated/.cache"),
                               .generator_version = 1});
    }
};

} // namespace

TEST_CASE("A cache key is exactly what it was", "[shader][compiler]") {
    // Every SPIR-V the compiler has cached is filed under this key. A change to
    // what goes into it recompiles every shader on every machine, so it has to
    // be a deliberate one: these values are FNV-1a over the include files,
    // the source, the defines, the stage and the generator's version, worked
    // out apart from the code.
    ShaderTree tree;
    tree.write("shaders/generated/x.frag", "void main() {}\n");
    const ShaderCompiler compiler = tree.compiler();

    const ShaderCompiler::Job shaped{tree.path("shaders/generated/x.frag"), "x", {"FJELL_UI_WORLD"}, "compute"};
    CHECK(compiler.cache_key(shaped) == 0x84d69e786dc99b33ULL);
    const ShaderCompiler::Job plain{tree.path("shaders/generated/x.frag"), "x"};
    CHECK(compiler.cache_key(plain) == 0x8baea5ec14873771ULL);
}

TEST_CASE("An optimised job is kept apart from the plain one", "[shader][compiler]") {
    ShaderTree tree;
    tree.write("shaders/generated/x.frag", "void main() {}\n");
    const ShaderCompiler compiler = tree.compiler();

    ShaderCompiler::Job job{tree.path("shaders/generated/x.frag"), "x"};
    const uint64_t plain = compiler.cache_key(job);
    job.optimize = true;
    CHECK(compiler.cache_key(job) != plain);
}

TEST_CASE("An edited include changes the key once it is hashed again", "[shader][compiler]") {
    ShaderTree tree;
    tree.write("shaders/generated/x.frag", "void main() {}\n");
    ShaderCompiler compiler = tree.compiler();
    const ShaderCompiler::Job job{tree.path("shaders/generated/x.frag"), "x"};
    const uint64_t before = compiler.cache_key(job);

    tree.write("shaders/include/tone/b.glsl", "// b, edited\n");
    CHECK(compiler.cache_key(job) == before);
    compiler.rehash_includes();
    CHECK(compiler.cache_key(job) != before);
}

TEST_CASE("Generated sources land in the generated directory", "[shader][compiler]") {
    ShaderTree tree;
    const ShaderCompiler compiler = tree.compiler();
    const std::string path = compiler.write_generated("sky.frag", "void main() {}\n");
    CHECK(path == tree.path("shaders/generated/sky.frag"));
    CHECK(std::filesystem::exists(path));
    CHECK(compiler.read_include("tone/b.glsl") == "// b\n");
    CHECK(compiler.read_include("nothing.glsl").empty());
}

TEST_CASE("An include comes from the first directory that has it", "[shader][compiler]") {
    ShaderTree tree;
    tree.write("game/include/a.glsl", "// the game's a\n");
    const ShaderCompiler compiler =
        tree.compiler({tree.path("game/include"), tree.path("shaders/include")});
    CHECK(compiler.read_include("a.glsl") == "// the game's a\n");
    CHECK(compiler.read_include("tone/b.glsl") == "// b\n");
    CHECK(compiler.read_include("nothing.glsl").empty());
}

TEST_CASE("Every include directory is in the key, in its order", "[shader][compiler]") {
    ShaderTree tree;
    tree.write("shaders/generated/x.frag", "void main() {}\n");
    const ShaderCompiler::Job job{tree.path("shaders/generated/x.frag"), "x"};
    const std::string game = tree.path("game/include");
    const std::string library = tree.path("shaders/include");
    const uint64_t alone = tree.compiler().cache_key(job);

    // A game with no include directory keeps what the library alone cached.
    CHECK(tree.compiler({game, library}).cache_key(job) == alone);

    // One file of its own changes every key.
    tree.write("game/include/c.glsl", "// c\n");
    ShaderCompiler compiler = tree.compiler({game, library});
    const uint64_t with_game = compiler.cache_key(job);
    CHECK(with_game != alone);

    // The same directories searched the other way round find other files
    // first, so they key differently.
    tree.write("game/include/a.glsl", "// the game's a\n");
    compiler.rehash_includes();
    const uint64_t game_first = compiler.cache_key(job);
    compiler.set_include_dirs({library, game});
    CHECK(compiler.cache_key(job) != game_first);
    CHECK(compiler.include_dirs() == std::vector<std::string>{library, game});
}

// glslc searches its -I directories in the order given, which has to be the
// order read_include() and the key take them in.
TEST_CASE("A compile takes an include from the first directory that has it", "[shader][compiler]") {
    std::string version;
    if (platform::run_command("glslc --version", version) != 0) SKIP("glslc not available");
    ShaderTree tree;
    tree.write("game/include/value.glsl", "const float VALUE = 2.0;\n");
    tree.write("shaders/include/value.glsl", "#error the library's value.glsl was taken\n");
    tree.write("shaders/generated/x.frag",
               "#version 460\n"
               "#include \"value.glsl\"\n"
               "#include \"tone/b.glsl\"\n"
               "layout(location = 0) out vec4 color;\n"
               "void main() { color = vec4(VALUE); }\n");
    const ShaderCompiler compiler =
        tree.compiler({tree.path("game/include"), tree.path("shaders/include")});
    const std::string spv = compiler.compile({tree.path("shaders/generated/x.frag"), "x"});
    CHECK(std::filesystem::exists(spv));
}

TEST_CASE("A compiler given no cache folder compiles every time and keeps nothing else", "[shader][compiler]") {
    std::string version;
    if (platform::run_command("glslc --version", version) != 0) SKIP("glslc not available");
    ShaderTree tree;
    tree.write("shaders/generated/x.frag",
               "#version 460\nlayout(location = 0) out vec4 color;\nvoid main() { color = vec4(1.0); }\n");
    const ShaderCompiler compiler({.include_dirs = {tree.path("shaders/include")},
                                   .generated_dir = tree.path("shaders/generated"),
                                   .cache_dir = {},
                                   .generator_version = 1});
    for (int i = 0; i < 2; ++i) CHECK(std::filesystem::exists(compiler.compile({tree.path("shaders/generated/x.frag"), "x"})));
    // The source and the SPIR-V the job asked for, and nothing kept beside.
    std::vector<std::string> written;
    for (const auto& entry : std::filesystem::directory_iterator(tree.path("shaders/generated"))) {
        written.push_back(entry.path().filename().string());
    }
    std::ranges::sort(written);
    CHECK(written == std::vector<std::string>{"x.frag", "x.spv"});
}

TEST_CASE("A compiler runs the program it is given, and fails with what was said when that isn't there",
          "[shader][compiler]") {
    ShaderTree tree;
    tree.write("shaders/generated/x.frag", "void main() {}\n");
    const ShaderCompiler compiler({.include_dirs = {tree.path("shaders/include")},
                                   .generated_dir = tree.path("shaders/generated"),
                                   .cache_dir = {},
                                   .generator_version = 1,
                                   .program = "ftk-no-such-glslc"});
    CHECK_THROWS_AS(compiler.compile({tree.path("shaders/generated/x.frag"), "x"}), std::runtime_error);
}
