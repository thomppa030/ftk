#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ftk {

/// Compiles GLSL to SPIR-V with glslc into `generated_dir`. Compiled SPIR-V
/// is kept in `generated_dir/.fjcache`, keyed by everything that produced it,
/// so a source compiled before is copied back rather than compiled again.
class ShaderCompiler {
public:
    /// `shader_dir` holds `include/`: what the compiled GLSL #includes, and
    /// what every cache key is hashed over. `generator_version` is the version
    /// of whatever writes the GLSL this compiles (see cache_key()). Creates
    /// `generated_dir` and its cache.
    ShaderCompiler(std::string shader_dir, std::string generated_dir, uint32_t generator_version);

    /// Hashes `include/` again, for after an include has changed: until then a
    /// shader that includes it finds its old SPIR-V under an unchanged key.
    void rehash_includes();

    /// The text of `include/<name>`, or empty when there is no such file.
    [[nodiscard]] std::string read_include(const std::string& name) const;

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
    /// deterministic, so the same key is the same SPIR-V. The generator's
    /// version is there for GLSL a program writes rather than an author: when
    /// what it writes changes shape, the file it writes from has not moved but
    /// its SPIR-V has, so the program bumps its version.
    [[nodiscard]] uint64_t cache_key(const Job& job) const;

private:
    std::string shader_dir_;
    std::string generated_dir_;
    std::string cache_dir_;
    uint32_t generator_version_;
    /// One hash over every file under include/, subdirectories too.
    ///
    /// Generated GLSL still #includes headers, so hashing the source alone
    /// would miss an edit to one of them. Asking glslc for each shader's real
    /// dependency list costs 58 ms, about what compiling costs, which defeats
    /// the point, while hashing all the includes together costs 1 ms. So an
    /// edit to any include invalidates every cached shader, which is what a
    /// template edit already does anyway.
    uint64_t include_hash_{0};
};

} // namespace ftk
