#pragma once

#include "renderer/resources/fjsl_parser.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace fjell {

class Project;

/// Whether a file of this kind is drawn as some ShaderType: a `.fjsl`, or a
/// `.fjmat` through the shader it names. Judged by extension alone.
[[nodiscard]] bool carries_shader_type(const std::filesystem::path& file);

/// The ShaderType an asset is drawn as, found without compiling anything: a
/// `.fjsl`'s own declaration, a `.fjmat`'s shader's. Nullopt for any other
/// file, a `shader_function` library, a material whose shader is missing,
/// or a file that cannot be read. `file` is absolute; a material's shader is
/// looked for in the project first and then the engine, as the pipeline
/// cache does.
///
/// Remembered per file and re-read when its timestamp moves, so an
/// inspector may ask every frame. Safe to call from any thread.
[[nodiscard]] std::optional<ShaderType> asset_shader_type(const std::filesystem::path& file,
                                                          const Project* project);

/// Whether a slot taking `accepted` may hold `file`. An empty list accepts
/// anything, as does a file that carries no ShaderType, so a slot's
/// extensions stay what decides everything else.
[[nodiscard]] bool shader_type_accepted(const std::filesystem::path& file, const Project* project,
                                        std::span<const ShaderType> accepted);

/// A slot's list when it takes one Type, which is most of them. Points into
/// static storage, so it can sit in a config for as long as it likes.
[[nodiscard]] std::span<const ShaderType> only_shader_type(ShaderType type);

/// "water", or "water or fog": what a slot taking `accepted` asks for.
[[nodiscard]] std::string describe_shader_types(std::span<const ShaderType> accepted);

} // namespace fjell
