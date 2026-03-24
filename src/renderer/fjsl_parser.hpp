#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

// Render mode parsed from .fjsl files
enum class RenderMode : uint8_t {
    Lit,    // default PBR lighting
    Unlit,  // skip lighting, output ALBEDO + EMISSION directly
};

// Uniform hint types parsed from .fjsl files
enum class UniformHint {
    None,
    Color,
    White,
    Black,
    Normal,
    DefaultMr,
};

struct HintRange {
    float min{0.0f};
    float max{1.0f};
    float step{0.01f};
};

// Uniform type as declared in .fjsl
enum class UniformType {
    Float,
    Vec2,
    Vec3,
    Vec4,
    Sampler2D,
};

// A parsed uniform declaration from .fjsl
struct FjslUniform {
    std::string name;
    UniformType type{UniformType::Float};
    UniformHint hint{UniformHint::None};
    HintRange range{};
    std::vector<float> default_value; // scalar or vector components
    uint32_t slot{0}; // texture slot (4-7) for sampler2D, or param offset for scalars
};

// Complete metadata extracted from a .fjsl file
struct ShaderMetadata {
    std::string shader_type; // "spatial"
    RenderMode render_mode{RenderMode::Lit};
    std::vector<FjslUniform> uniforms;
    std::string fragment_body; // contents of fragment() { ... }
    uint32_t custom_texture_count{0};
    uint32_t custom_param_count{0}; // total floats needed for scalar uniforms
};

// Parse a .fjsl file and extract metadata.
[[nodiscard]] ShaderMetadata parse_fjsl(const std::string& source);

// Generate a complete .frag GLSL file from the spatial template + FJSL metadata.
[[nodiscard]] std::string generate_fragment_shader(const ShaderMetadata& meta,
                                                    const std::string& template_source);

// Return the number of floats consumed by a uniform type.
[[nodiscard]] uint32_t uniform_float_count(UniformType type);

} // namespace fjell
