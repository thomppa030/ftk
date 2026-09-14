#pragma once

#include "renderer/resources/fjsl_parser.hpp"

#include <glm/glm.hpp>

#include <functional>
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
    /// The material's emission factor. The visible path multiplies the
    /// emissive texture by it, the GI paths multiply the texture by it when
    /// one is bound and use it alone otherwise, so both read one value.
    ///
    /// Product of every hint_emission uniform when the shader declares any.
    /// Otherwise one when the material bound an emissive texture, so the
    /// texture shows at full strength, and zero when it did not, so nothing
    /// emits.
    glm::vec3 emission{0.0F};

    /// The material's albedo factor: the product of every hint_albedo
    /// uniform, one when the shader tags none. Multiplies the base colour
    /// texture on every path, so a solid-colour shader that tags its tint
    /// bounces that tint in the trace shaders, which cannot run fragment().
    glm::vec3 albedo{1.0F};
};

/// Current floats for a non-sampler uniform: what the material stores for
/// it, or the shader default when it stores nothing. An empty result means
/// the value is unavailable and the uniform is skipped.
using UniformValueReader = std::function<std::vector<float>(const FjslUniform&)>;

/// `has_emissive_texture` is whether the material bound one; it decides what
/// an untagged shader's factor defaults to.
[[nodiscard]] MaterialGiParams derive_material_gi_params(const ShaderMetadata& meta,
                                                          const UniformValueReader& read,
                                                          bool has_emissive_texture);

} // namespace fjell
