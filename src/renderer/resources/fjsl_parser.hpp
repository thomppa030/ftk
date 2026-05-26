#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

// Lighting mode parsed from .fjsl files
enum class RenderMode : uint8_t {
    Lit,    // default PBR lighting
    Unlit,  // skip lighting, output ALBEDO + EMISSION directly
};

// Blend mode for materials
enum class BlendMode : uint8_t {
    Alpha,     // default alpha blending for transparent
    Add,       // additive (particles, holograms)
    Multiply,  // multiplicative
};

// Per-material render state parsed from render_mode keywords
struct RenderState {
    bool cull_disabled{false};
    bool cull_front{false};        // front-face culling (e.g. outline pass)
    bool depth_write_disabled{false};
    bool depth_test_disabled{false};
    bool force_transparent{false}; // explicit blend_alpha/blend_add/blend_multiply
    BlendMode blend{BlendMode::Alpha};
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
    RenderState render_state;
    std::vector<FjslUniform> uniforms;
    std::string fragment_body; // contents of fragment() { ... }
    std::string vertex_body;   // contents of vertex() { ... } (empty = standard)
    std::string light_body;    // contents of light() { ... } (empty = default PBR)
    std::string user_globals;  // top-level helpers / constants the user authored
    uint32_t custom_texture_count{0};
    uint32_t custom_param_count{0}; // total floats needed for scalar uniforms
};

// Parse a .fjsl file and extract metadata.
[[nodiscard]] ShaderMetadata parse_fjsl(const std::string& source);

// Generate a complete .frag GLSL file from the spatial template + FJSL metadata.
[[nodiscard]] std::string generate_fragment_shader(const ShaderMetadata& meta,
                                                    const std::string& template_source);

// Generate a .vert or .mesh GLSL file from a vertex/mesh template + FJSL metadata.
// Used for both vertex shaders and mesh shaders — same placeholder substitution.
[[nodiscard]] std::string generate_vertex_shader(const ShaderMetadata& meta,
                                                  const std::string& template_source);

// Return the number of floats consumed by a uniform type.
[[nodiscard]] uint32_t uniform_float_count(UniformType type);

} // namespace fjell
