#include "renderer/resources/material_gi_params.hpp"

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
bool emission_from_hints(const ShaderMetadata& meta, const UniformValueReader& read,
                         glm::vec3& out) {
    glm::vec3 product{1.0F};
    bool any = false;
    for (const auto& u : meta.uniforms) {
        if (!u.emission || u.type == UniformType::Sampler2D) continue;
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

// Pre-hint materials named their uniforms emission_color / emission_strength
// (or glow_*) and were recognised by that name alone. Still honoured so they
// keep emitting, but a rename silently changes lighting, which hint_emission
// exists to prevent.
bool emission_from_names(const ShaderMetadata& meta, const UniformValueReader& read,
                         glm::vec3& out) {
    float strength = 0.0F;
    std::optional<glm::vec3> colour;
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;
        if (u.name == "emission_strength" || u.name == "glow_strength") {
            auto values = read(u);
            if (!values.empty()) strength = values[0];
        } else if (u.name == "glow_color" || u.name == "emission_color") {
            colour = vec3_of(read(u));
        }
    }

    if (colour && strength > 0.0F) {
        out = *colour * strength;
    } else if (colour) {
        out = *colour;
    } else if (strength > 0.0F) {
        out = glm::vec3{strength};
    } else {
        return false;
    }
    return true;
}

// Unambiguous only when the shader has exactly one hint_color vector: that
// is "the" surface colour by construction of the hint. Zero or several leave
// the trace shaders on the base colour, since there is no way to tell which
// one, if any, feeds ALBEDO.
std::optional<glm::vec3> albedo_override(const ShaderMetadata& meta,
                                         const UniformValueReader& read) {
    const FjslUniform* sole = nullptr;
    int count = 0;
    for (const auto& u : meta.uniforms) {
        if (u.hint == UniformHint::Color
            && (u.type == UniformType::Vec3 || u.type == UniformType::Vec4)) {
            sole = &u;
            ++count;
        }
    }
    if (count != 1) return std::nullopt;
    return vec3_of(read(*sole));
}

} // namespace

MaterialGiParams derive_material_gi_params(const ShaderMetadata& meta,
                                           const UniformValueReader& read) {
    MaterialGiParams out;
    if (!emission_from_hints(meta, read, out.emission)) {
        out.emission_from_names = emission_from_names(meta, read, out.emission);
    }
    out.gi_albedo_override = albedo_override(meta, read);
    return out;
}

} // namespace fjell
