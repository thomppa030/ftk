#pragma once

#include "renderer/resources/fjsl_parser.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <vector>

namespace fjell {

/// What the GI paths can read of a material.
///
/// DDGI, SS-GI and RT reflections hit surfaces from a ray, in a shader that
/// cannot run the material's fragment(). They read a per-material summary
/// instead, and this is where that summary is derived — once, for both the
/// asset loader and the live material inspector, so a value edited in the
/// editor is what a fresh load of the same values would produce.
struct MaterialGiParams {
    /// Product of every hint_emission uniform. Zero when the shader declares
    /// none, which reads as "does not emit" everywhere it is consumed.
    glm::vec3 emission{0.0F};

    /// The surface colour, when the shader sources ALBEDO from a single
    /// hint_color uniform rather than the base colour texture. Without it
    /// the trace shaders bounce base_color_factor, which such shaders leave
    /// at its white default.
    std::optional<glm::vec3> gi_albedo_override;

    /// Emission came from matching uniform names rather than a hint_emission
    /// tag. Kept so callers can point at shaders still relying on it; the
    /// name match exists only to keep existing materials working.
    bool emission_from_names{false};
};

/// Current floats for a non-sampler uniform: what the material stores for
/// it, or the shader default when it stores nothing. An empty result means
/// the value is unavailable and the uniform is skipped.
using UniformValueReader = std::function<std::vector<float>(const FjslUniform&)>;

[[nodiscard]] MaterialGiParams derive_material_gi_params(const ShaderMetadata& meta,
                                                          const UniformValueReader& read);

} // namespace fjell
