#pragma once

#include "renderer/resources/fjsl_parser.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

/// The version of what FJSL generates, which every compiled shader's cache
/// key carries (ShaderCompiler::cache_key()). The generated GLSL is an output
/// of fjsl_parser.cpp, so changing how that emits must invalidate everything:
/// the .fjsl did not move but what it produces did. Bump it whenever
/// generation changes shape.
inline constexpr uint32_t FJELL_SHADER_CODEGEN_VERSION = 3;

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

/// Reads a .fjsl through the host's locator, parses it and composes the
/// functions it uses: what its GLSL is generated from. A ShaderCompiler then
/// compiles what is generated.
class FjslLoader {
public:
    explicit FjslLoader(FjslLocator locator);

    /// Reads, parses and composes the .fjsl `reference` names. Warnings go to
    /// the log. Throws std::runtime_error carrying every error as
    /// file:line:column: message when the program cannot be built from.
    [[nodiscard]] FjslProgram load(const std::string& reference) const;

private:
    FjslLocator locator_;
};

} // namespace fjell
