#include "ftk/shader/shader_compiler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ftk;

namespace {

// A shader tree in a directory of its own: include/ for the compiler to hash
// and generated/ for what it writes.
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
    // are the ones its SPIR-V cache holds.
    [[nodiscard]] ShaderCompiler compiler() const {
        return ShaderCompiler({.shader_dir = path("shaders"),
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
    CHECK(compiler.cache_key(shaped) == 0x7e76e492e9072f89ULL);
    const ShaderCompiler::Job plain{tree.path("shaders/generated/x.frag"), "x"};
    CHECK(compiler.cache_key(plain) == 0xbae69c53d4f95297ULL);
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
