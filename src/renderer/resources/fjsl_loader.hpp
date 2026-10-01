#pragma once

#include "renderer/resources/fjsl_parser.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace fjell {

/// The version of what FJSL generates, which every compiled shader's cache
/// key carries (ShaderCompiler::cache_key()). The generated GLSL is an output
/// of fjsl_parser.cpp, so changing how that emits must invalidate everything:
/// the .fjsl did not move but what it produces did. Bump it whenever
/// generation changes shape.
inline constexpr uint32_t FJELL_SHADER_CODEGEN_VERSION = 3;

/// Where the file a shader reference names is on disk, or empty when it is
/// nowhere the host looks. A reference is what a material wrote: a relative
/// path, or whatever else the host resolves. The engine looks in the open
/// project, then in its own tree, and finds an asset id through the project's
/// database.
using FjslLocator = std::function<std::string(const std::string& reference)>;

/// One .fjsl read and parsed: what its GLSL is generated from
/// (generate_fragment_shader(), generate_vertex_shader()).
struct FjslProgram {
    /// The reference it was loaded from.
    std::string path;
    /// What the parse found: the Type, the uniforms, the hooks and the code.
    ShaderMetadata metadata;

    /// Whether a change to `changed`, an absolute path as a file watcher
    /// reports it, reaches this program: its own file. A relative reference
    /// matches the end of `changed` on a path boundary, so "water.fjsl" is not
    /// "deep_water.fjsl". An include it reads is not asked about here: the
    /// watcher rebuilds every program when an include changes.
    [[nodiscard]] bool depends_on(std::string_view changed) const;
};

/// Reads a .fjsl through the host's locator and parses it. A ShaderCompiler
/// then compiles what is generated from it.
class FjslLoader {
public:
    explicit FjslLoader(FjslLocator locator);

    /// Reads and parses the .fjsl `reference` names. Warnings go to the log.
    /// Throws std::runtime_error carrying every error as
    /// file:line:column: message when the program cannot be built from.
    [[nodiscard]] FjslProgram load(const std::string& reference) const;

private:
    FjslLocator locator_;
};

} // namespace fjell
