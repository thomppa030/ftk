#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ftk {

/// Where a ShaderCompiler reads and writes.
struct ShaderCompilerDesc {
    /// Where `#include` looks, in order: a file is taken from the first
    /// directory that has it, so a program's own includes can come ahead of
    /// a library's. A directory that does not exist is passed over. Every
    /// file under these is hashed into every cache key.
    std::vector<std::string> include_dirs;
    /// Where generated GLSL and compiled SPIR-V are written.
    std::string generated_dir;
    /// Where compiled SPIR-V is kept between runs, by its cache key. Empty
    /// keeps nothing: every job is compiled, for a program that compiles to
    /// check and has no use for the SPIR-V later.
    std::string cache_dir;
    /// The version of whatever writes the GLSL this compiles (see
    /// ShaderCompiler::cache_key()).
    uint32_t generator_version{0};
    /// The compiler it runs: a name looked up on the PATH, or a path.
    std::string program{"glslc"};
};

/// Compiles GLSL to SPIR-V with glslc into the generated directory. Compiled
/// SPIR-V is kept in the cache directory, when there is one, keyed by
/// everything that produced it, so a source compiled before is copied back
/// rather than compiled again.
class ShaderCompiler {
public:
    /// Creates the generated and cache directories.
    explicit ShaderCompiler(ShaderCompilerDesc desc);

    /// Hashes the include directories again, for after an include has
    /// changed or a directory has appeared: until then a shader that includes
    /// it finds its old SPIR-V under an unchanged key. A compiler that keeps
    /// no cache hashes nothing.
    void rehash_includes();

    /// Looks for includes in `dirs` from now on, in that order, and hashes
    /// them.
    void set_include_dirs(std::vector<std::string> dirs);

    /// Where `#include` looks, in order.
    [[nodiscard]] const std::vector<std::string>& include_dirs() const { return include_dirs_; }

    /// The text of `name` from the first include directory that has it, or
    /// empty when none does.
    [[nodiscard]] std::string read_include(const std::string& name) const;

    /// Writes generated GLSL to `generated_dir/<name>` and returns its path.
    /// Throws std::runtime_error when it cannot.
    [[nodiscard]] std::string write_generated(const std::string& name, const std::string& source) const;

    /// One GLSL source to compile.
    struct Job {
        std::string source_path;
        /// The SPIR-V goes to `generated_dir/<output_name>.spv`; what
        /// preprocess() reads, to `generated_dir/<output_name>.i` while glslc
        /// runs.
        std::string output_name;
        /// Passed as -D, for a source compiled in more than one shape.
        std::vector<std::string> defines{};
        /// glslc's -fshader-stage; empty infers it from the extension.
        std::string stage{};
        /// glslc's -O: code whose result nothing uses, and the resources only
        /// it read, are dropped, and the names stay. For a shape of a source
        /// that uses part of what the source computes.
        bool optimize{false};
    };

    /// Compiles `job` and returns the SPIR-V's path, from the cache when the
    /// same source was compiled before. Throws std::runtime_error carrying
    /// glslc's output when it fails.
    [[nodiscard]] std::string compile(const Job& job) const;

    /// Compiles every job at once and returns the SPIR-V paths in the jobs'
    /// order. Throws the first failure, in that order, once all have ended.
    [[nodiscard]] std::vector<std::string> compile_all(std::span<const Job> jobs) const;

    /// `job`'s source as glslc preprocesses it with the flags it is compiled
    /// with: its macros expanded and its includes taken in, a
    /// `#line N "file"` before each run of lines saying where they are from.
    /// Nothing is compiled or kept. Throws std::runtime_error carrying
    /// glslc's output when it fails.
    [[nodiscard]] std::string preprocess(const Job& job) const;

    /// What `job`'s SPIR-V is kept under: a hash of the includes, the source,
    /// the defines, the stage, whether it is optimised (only when it is, so
    /// an unoptimised job keeps the key it always had) and the generator's
    /// version. glslc is
    /// deterministic, so the same key is the same SPIR-V. The generator's
    /// version is there for GLSL a program writes rather than an author: when
    /// what it writes changes shape, the file it writes from has not moved but
    /// its SPIR-V has, so the program bumps its version.
    [[nodiscard]] uint64_t cache_key(const Job& job) const;

private:
    /// glslc with what every run of `job` passes it: the target, the include
    /// directories, the stage and the defines.
    [[nodiscard]] std::string glslc(const Job& job) const;

    std::vector<std::string> include_dirs_;
    std::string generated_dir_;
    std::string cache_dir_;
    uint32_t generator_version_;
    std::string program_;
    /// One hash over every file under the include directories, their
    /// subdirectories too.
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
