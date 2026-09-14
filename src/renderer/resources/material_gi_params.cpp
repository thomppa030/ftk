#include "renderer/resources/material_gi_params.hpp"

#include <optional>

namespace fjell {

namespace {

std::optional<glm::vec3> vec3_of(const std::vector<float>& v) {
    if (v.size() < 3) return std::nullopt;
    return glm::vec3{v[0], v[1], v[2]};
}

// The tagged uniforms multiply together: a colour and its strength, in
// whatever combination the shader declares. A shader whose emission is a sum
// of several terms tags the dominant one, since a product cannot express the
// others.
bool product_of_tagged(const ShaderMetadata& meta, const UniformValueReader& read,
                       bool FjslUniform::*tag, glm::vec3& out) {
    glm::vec3 product{1.0F};
    bool any = false;
    for (const auto& u : meta.uniforms) {
        if (!(u.*tag) || u.type == UniformType::Sampler2D) continue;
        auto values = read(u);
        if (values.empty()) continue;
        if (u.type == UniformType::Float) {
            product *= values[0];
        } else if (auto rgb = vec3_of(values)) {
            product *= *rgb;
        } else {
            continue;
        }
        any = true;
    }
    if (any) out = product;
    return any;
}

} // namespace

MaterialGiParams derive_material_gi_params(const ShaderMetadata& meta,
                                           const UniformValueReader& read,
                                           bool has_emissive_texture) {
    MaterialGiParams out;
    if (!product_of_tagged(meta, read, &FjslUniform::emission, out.emission)) {
        // Nothing declared: a bound texture is the emission, shown as
        // authored; without one the material does not emit.
        out.emission = glm::vec3{has_emissive_texture ? 1.0F : 0.0F};
    }
    // An untagged shader keeps the factor at one: the base colour texture
    // and the file's own base colour are the albedo.
    product_of_tagged(meta, read, &FjslUniform::albedo, out.albedo);
    return out;
}

} // namespace fjell
