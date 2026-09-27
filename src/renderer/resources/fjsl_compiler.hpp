#pragma once

#include "renderer/resources/fjsl_parser.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

/// Where the file a shader reference names is on disk, or empty when it is
/// nowhere the host looks. A reference is what a material or a `use` wrote:
/// a relative path, or whatever else the host resolves. The engine looks in
/// the open project, then in its own tree, and finds an asset id through the
/// project's database.
using FjslLocator = std::function<std::string(const std::string& reference)>;

/// One .fjsl read, parsed and composed: what its GLSL is generated from.
struct FjslProgram {
    /// The reference it was loaded from.
    std::string path;
    /// The file's own metadata. Once generate_fragment() has run, the whole
    /// program's: the dials of every function it composes are folded in.
    ShaderMetadata metadata;
    /// What its `use` declarations resolved to; empty when it has none.
    FjslComposition composition;
    /// The shader_functions it composes, by the path each `use` wrote.
    std::vector<std::string> used_paths;

    /// Whether a change to `changed`, an absolute path as a file watcher
    /// reports it, reaches this program: its own file, or a function it
    /// composes, whose code is inlined into it. A relative reference matches
    /// the end of `changed` on a path boundary, so "water.fjsl" is not
    /// "deep_water.fjsl".
    [[nodiscard]] bool depends_on(std::string_view changed) const;
};

/// The program's GLSL, generated from `tmpl`, with the functions it composes
/// emitted ahead of its own code. Folds the composed dials into the program's
/// metadata afterwards, under the names the GLSL gave them, so the metadata
/// then describes everything the program reads: the inspector lists its dials
/// from it and the passes size the parameter block from it. Throws
/// std::runtime_error when the composition wants more textures than a
/// material has room for.
///
/// Generate each program once: the generator lays each composed file out from
/// the consumer's own counts, which the fold then grows.
[[nodiscard]] std::string generate_fragment(FjslProgram& program, const std::string& tmpl);

/// Turns .fjsl into SPIR-V: reads a shader through the host's locator, parses
/// and composes it, and compiles generated GLSL with glslc into
/// `generated_dir`. Compiled SPIR-V is kept in `generated_dir/.fjcache`, keyed
/// by everything that produced it, so a source compiled before is copied back
/// rather than compiled again.
class FjslCompiler {
public:
    /// `shader_dir` holds `include/`: what generated GLSL #includes, and what
    /// every cache key is hashed over. Creates `generated_dir` and its cache.
    FjslCompiler(std::string shader_dir, std::string generated_dir, FjslLocator locator);

    /// Hashes `include/` again, for after an include has changed: until then a
    /// shader that includes it finds its old SPIR-V under an unchanged key.
    void rehash_includes();

    /// The text of `include/<name>`, or empty when there is no such file.
    /// Templates live there.
    [[nodiscard]] std::string read_include(const std::string& name) const;

    /// Reads, parses and composes the .fjsl `reference` names. Warnings go to
    /// the log. Throws std::runtime_error carrying every error as
    /// file:line:column: message when the program cannot be built from.
    [[nodiscard]] FjslProgram load(const std::string& reference) const;

    /// Writes generated GLSL to `generated_dir/<name>` and returns its path.
    /// Throws std::runtime_error when it cannot.
    [[nodiscard]] std::string write_generated(const std::string& name, const std::string& source) const;

    /// One GLSL source to compile.
    struct Job {
        std::string source_path;
        /// The SPIR-V goes to `generated_dir/<output_name>.spv`.
        std::string output_name;
        /// Passed as -D, for a source compiled in more than one shape.
        std::vector<std::string> defines{};
        /// glslc's -fshader-stage; empty infers it from the extension.
        std::string stage{};
    };

    /// Compiles `job` and returns the SPIR-V's path, from the cache when the
    /// same source was compiled before. Throws std::runtime_error carrying
    /// glslc's output when it fails.
    [[nodiscard]] std::string compile(const Job& job) const;

    /// Compiles every job at once and returns the SPIR-V paths in the jobs'
    /// order. Throws the first failure, in that order, once all have ended.
    [[nodiscard]] std::vector<std::string> compile_all(std::span<const Job> jobs) const;

    /// What `job`'s SPIR-V is kept under: a hash of the includes, the source,
    /// the defines, the stage and the generator's version. glslc is
    /// deterministic, so the same key is the same SPIR-V.
    [[nodiscard]] uint64_t cache_key(const Job& job) const;

private:
    std::string shader_dir_;
    std::string generated_dir_;
    std::string cache_dir_;
    FjslLocator locator_;
    /// One hash over every file under include/, subdirectories too.
    ///
    /// The generated GLSL still #includes engine headers, so hashing it
    /// alone would miss an edit to one of them. Asking glslc for each
    /// shader's real dependency list costs 58 ms, about what compiling
    /// costs, which defeats the point, while hashing all the includes
    /// together costs 1 ms. So an edit to any include invalidates every
    /// cached shader, which is what a template edit already does anyway.
    uint64_t include_hash_{0};
};

} // namespace fjell
